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
	for (const auto& kv : agent.activeAsks) {
		const std::shared_ptr<Order>& order = kv.second;
		if (order != nullptr && order->status == OrderStatus::OPEN) { shares += order->volume; }
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
	(void)agent;
	return false;
}

double equity(const Agent& agent, double price) {
	return agent.cash + escrowedCash(agent) + (double(longShares(agent)) - double(agent.shortShares)) * price;
}

double buyingPower(const Agent& agent) {
	if (!hasMarginPrivileges(agent)) { return std::max(agent.cash, 0.0); }

	double price = agent.OB.currentPrice;
	if (!isMarginable(price)) { return std::max(agent.cash, 0.0); }

	// Reg T: what is held and what is already bid for both need initial margin
	double committed = double(longShares(agent)) * price + escrowedCash(agent);
	double excess = equity(agent, price) - REG_T_INITIAL * committed;
	return std::max(excess, 0.0) / REG_T_INITIAL;
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
	return longMaintenance(agent, price) * double(longShares(agent)) * price;
}

bool inMaintenanceViolation(const Agent& agent, double price) {
	// Only a borrowed position can be in violation. A long bought outright is worth at least
	// itself, and 100% of itself is the most any requirement asks.
	if (debitBalance(agent) <= 0.0) { return false; }
	return equity(agent, price) < maintenanceRequirement(agent, price) - 1e-9;
}

double liquidationPrice(const Agent& agent) {
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
