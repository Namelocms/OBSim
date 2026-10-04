#include "include/Account.h"
#include "include/Agent.h"
#include "include/Order.h"
#include "include/Holding.h"
#include "include/Enums.h"
#include "include/OrderBook.h"
#include "include/FeeSchedule.h"
#include <climits>
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
	return (long long)longShares(agent);
}

bool canSellShort(const Agent& agent) {
	(void)agent;
	return false;
}

double equity(const Agent& agent, double price) {
	return agent.cash + escrowedCash(agent) + double(longShares(agent)) * price;
}

double buyingPower(const Agent& agent) {
	return std::max(agent.cash, 0.0);
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
