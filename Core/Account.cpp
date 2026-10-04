#include "include/Account.h"
#include "include/Agent.h"
#include "include/Order.h"
#include "include/Holding.h"
#include "include/Enums.h"
#include "include/OrderBook.h"
#include "include/FeeSchedule.h"
#include <climits>
#include <cmath>
#include <algorithm>

namespace Account {

double escrowedCash(const Agent& agent) {
	double escrow = 0.0;
	for (const auto& kv : agent.activeBids) {
		const std::shared_ptr<Order>& order = kv.second;
		if (order != nullptr && order->status == OrderStatus::OPEN) { escrow += order->price * order->volume + order->feeReserve; }
	}
	return escrow;
}

unsigned long long longShares(const Agent& agent) {
	unsigned long long shares = 0;
	for (const auto& kv : agent.holdings) { shares += kv.second.volume; }
	// Only a LONG ask's shares are owned: they came out of holdings into the order. A short
	// sale offers shares the account does not have.
	for (const auto& kv : agent.activeAsks) {
		const std::shared_ptr<Order>& order = kv.second;
		if (order != nullptr && order->status == OrderStatus::OPEN && !order->isShortSale()) { shares += order->volume; }
	}
	return shares;
}

long long netShares(const Agent& agent) {
	return (long long)longShares(agent) - (long long)agent.shortShares;
}

bool shortingEnabled(const Agent& agent) {
	return agent.OB.features.shorting.enabled && agent.OB.features.margin.enabled;
}

bool canSellShort(const Agent& agent) {
	if (!shortingEnabled(agent)) { return false; }
	if (agent.getTotalHoldings() > 0) { return false; }   // a long sells down to flat first
	if (shortCapacity(agent) < 1) { return false; }
	return isExemptMarketMaker(agent) || agent.OB.lending.available() >= 1.0;
}

double equity(const Agent& agent, double price) {
	return agent.cash + escrowedCash(agent) + (double(longShares(agent)) - double(agent.shortShares)) * price;
}

double buyingPower(const Agent& agent) {
	const bool privileged = hasMarginPrivileges(agent);
	const unsigned long long shortExposure = agent.shortShares + pendingShortShares(agent);
	// A plain cash account, exactly as it always was
	if (!privileged && shortExposure == 0) { return std::max(agent.cash, 0.0); }

	double price = agent.OB.currentPrice;
	double rLong = (privileged && isMarginable(price)) ? REG_T_INITIAL : 1.0;
	if (rLong >= 1.0 && shortExposure == 0) { return std::max(agent.cash, 0.0); }

	// Reg T: what is held, what is already bid for, and every short (open or offered) all
	// need their initial margin out of the account's equity
	double committed = rLong * (double(longShares(agent)) * price + escrowedCash(agent))
		+ shortOpeningPerShare(price) * double(shortExposure);
	double excess = equity(agent, price) - committed;
	return std::max(excess, 0.0) / rLong;
}

bool hasMarginPrivileges(const Agent& agent) {
	if (!agent.OB.features.margin.enabled) { return false; }
	double scale = (agent.OB.cashScale > 0.0) ? agent.OB.cashScale : 1.0;
	return equity(agent, agent.OB.currentPrice) / scale >= MARGIN_MIN_EQUITY;
}

bool isMarginable(double price) {
	return price >= MARGINABLE_MIN_PRICE;
}

double longMaintenance(const Agent& agent, double price) {
	if (!isMarginable(price)) { return 1.0; }
	return std::max(FINRA_MAINTENANCE_LONG, feeSchedule(agent).houseMaintenance);
}

double maintenanceRequirement(const Agent& agent, double price) {
	return longMaintenance(agent, price) * double(longShares(agent)) * price
		+ shortMaintenancePerShare(price) * double(agent.shortShares);
}

bool inMaintenanceViolation(const Agent& agent, double price) {
	// Only a borrowed position can be in violation: a margin loan, or borrowed stock. A long
	// bought outright is worth at least itself, and 100% of itself is the most asked of it.
	if (debitBalance(agent) <= 0.0 && agent.shortShares == 0) { return false; }
	return equity(agent, price) < maintenanceRequirement(agent, price) - 1e-9;
}

double liquidationPrice(const Agent& agent) {
	if (agent.shortShares > 0) { return 0.0; }   // a short's trigger is above, see shortLiquidationPrice
	double c = agent.cash + escrowedCash(agent);
	unsigned long long shares = longShares(agent);
	if (c >= 0.0 || shares == 0) { return 0.0; }

	double m = std::max(FINRA_MAINTENANCE_LONG, feeSchedule(agent).houseMaintenance);
	double marginableTrigger = -c / (double(shares) * (1.0 - m));
	// Below the marginable line any loan is in violation, so the trigger is never below it
	return std::max(marginableTrigger, MARGINABLE_MIN_PRICE);
}

double debitBalance(const Agent& agent) {
	return std::max(-(agent.cash + escrowedCash(agent)), 0.0);
}

double shortMaintenancePerShare(double price) {
	if (price < MARGINABLE_MIN_PRICE) { return std::max(SHORT_LOW_PRICE_FLOOR, price); }
	return std::max(SHORT_HIGH_PRICE_FLOOR, SHORT_MAINTENANCE_PCT * price);
}

double shortOpeningPerShare(double price) {
	return std::max(REG_T_INITIAL * price, shortMaintenancePerShare(price));
}

unsigned long long pendingShortShares(const Agent& agent) {
	unsigned long long n = 0;
	for (const auto& kv : agent.activeAsks) {
		const std::shared_ptr<Order>& order = kv.second;
		if (order != nullptr && order->status == OrderStatus::OPEN && order->isShortSale()) { n += order->volume; }
	}
	return n;
}

unsigned int shortCapacity(const Agent& agent) {
	if (!shortingEnabled(agent) || !hasMarginPrivileges(agent)) { return 0; }
	double price = agent.OB.currentPrice;
	if (!(price > 0.0)) { return 0; }

	double rLong = isMarginable(price) ? REG_T_INITIAL : 1.0;
	double committed = rLong * (double(longShares(agent)) * price + escrowedCash(agent))
		+ shortOpeningPerShare(price) * double(agent.shortShares + pendingShortShares(agent));
	double excess = equity(agent, price) - committed;
	if (excess <= 0.0) { return 0; }
	double shares = excess / shortOpeningPerShare(price);
	return (shares >= double(INT_MAX)) ? (unsigned int)INT_MAX : (unsigned int)shares;
}

bool isExemptMarketMaker(const Agent& agent) {
	return agent.type == AgentType::INSTITUTION && agent.subType == AgentSubType::ALGO;
}

unsigned int coverableShares(const Agent& agent) {
	unsigned long long bidding = 0;
	for (const auto& kv : agent.activeBids) {
		if (kv.second != nullptr && kv.second->status == OrderStatus::OPEN) { bidding += kv.second->volume; }
	}
	return (bidding >= agent.shortShares) ? 0u : (unsigned int)(agent.shortShares - bidding);
}

double shortLiquidationPrice(const Agent& agent) {
	if (agent.shortShares == 0) { return 0.0; }

	// Equity falls and the requirement rises with the price, so violation is monotonic:
	// below some price the account is fine, above it it is not. Bisect for that price.
	auto violated = [&agent](double p) { return equity(agent, p) < maintenanceRequirement(agent, p); };
	double lo = 1e-4, hi = std::max(agent.OB.currentPrice, 1e-4) * 2.0;
	if (violated(lo)) { return lo; }
	while (!violated(hi)) {
		hi *= 2.0;
		if (hi > 1e9) { return 0.0; }   // cannot be called at any sane price
	}
	for (int i = 0; i < 80 && (hi - lo) > 1e-9 * hi; ++i) {
		double mid = 0.5 * (lo + hi);
		if (violated(mid)) { hi = mid; } else { lo = mid; }
	}
	return hi;
}

unsigned int shortCoverShares(const Agent& agent, double price) {
	unsigned int s = agent.shortShares;
	if (s == 0 || !(price > 0.0)) { return 0; }
	double e = equity(agent, price);
	if (e <= 0.0) { return s; }

	// Buying q at P leaves equity unchanged and S - q short, so the opening requirement
	// r(P) * (S - q) <= E needs q >= S - E / r(P)
	double q = std::ceil(double(s) - e / shortOpeningPerShare(price) - 1e-9);
	if (q < 1.0) { q = 1.0; }
	if (q > double(s)) { q = double(s); }
	return (unsigned int)q;
}

unsigned int liquidationShares(const Agent& agent, double price) {
	unsigned long long held = agent.getTotalHoldings();
	if (held == 0 || !(price > 0.0)) { return 0; }
	double e = equity(agent, price);
	if (e <= 0.0) { return (unsigned int)held; }

	// Selling q at P leaves equity unchanged and L - q shares, so the target r*(L - q)*P <= E
	// needs q >= L - E / (r * P). Not marginable means the target is the whole loan.
	double r = isMarginable(price) ? LIQUIDATION_TARGET : 1.0;
	double q = std::ceil(double(held) - e / (r * price) - 1e-9);
	if (q < 1.0) { q = 1.0; }
	if (q > double(held)) { q = double(held); }
	return (unsigned int)q;
}

const FeeSchedule& feeSchedule(const Agent& agent) {
	const Features::Fees& fees = agent.OB.features.fees;
	return (agent.type == AgentType::INSTITUTION) ? fees.institution : fees.retail;
}

bool feesEnabled(const Agent& agent) {
	return agent.OB.features.fees.enabled;
}

void releaseFeeReserve(Agent& agent, Order& order) {
	if (order.feeReserve > 0.0) { agent.updateCash(order.feeReserve); }
	order.feeReserve = 0.0;
}

unsigned int affordableVolume(const Agent& agent, double price) {
	if (!(price > 0.0)) { return 0; }
	double shares;
	if (feesEnabled(agent)) {
		const FeeSchedule& fees = feeSchedule(agent);
		double spendable = buyingPower(agent) - fees.buyFeeFixed() - 0.02;
		shares = (spendable > 0.0) ? spendable / (price + fees.buyFeePerShare(price)) : 0.0;
	}
	else {
		shares = buyingPower(agent) / price;
	}
	if (shares >= double(INT_MAX)) { return (unsigned int)INT_MAX; }
	return (unsigned int)shares;
}

}
