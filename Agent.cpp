#include "Agent.h"
#include "Enums.h"
#include "Util.h"
#include "Order.h"
#include "Holding.h"
#include "OrderBook.h"
#include "MatchingEngine.h"

Agent::Agent(std::string id, double cash, OrderBook& ob, MatchingEngine& me) :
	id(id), cash(cash), OB(ob), ME(me) {}

// ---- Cash Operations ----
void Agent::updateCash(double amt) {
	this->cash = roundTo(this->cash + amt, CASH_PRECISION);
}

// ---- Holdings Operations ----
void Agent::upsertHolding(Holding holding) {
	// Insert new holding if price key does not already exist
	auto attempt = this->holdings.try_emplace(holding.price, holding);
	
	auto it = attempt.first;
	auto inserted = attempt.second;

	// Not inserted so price key existed, just add volume
	if (!inserted) {
		it->second.volume += holding.volume;
	}
}
std::vector<Holding> Agent::removeHoldings(int volume) {
	std::vector<Holding> removedHoldings;

	while (volume > 0 && !this->holdings.empty()) {
		auto it = this->holdings.begin();
		Holding& currentHolding = it->second;

		// Clean faulty holdings if there are any
		if (currentHolding.volume <= 0) {
			this->holdings.erase(it);
			continue;
		}

		if (currentHolding.volume <= volume) {
			// Fully remove holding
			volume -= currentHolding.volume;
			removedHoldings.push_back(std::move(currentHolding));
			this->holdings.erase(it);
		}
		else {
			removedHoldings.emplace_back(currentHolding.price, volume);
			currentHolding.volume -= volume;
			volume = 0;
		}
	}

	return removedHoldings;
}
int Agent::getTotalHoldings() {
	return std::accumulate(this->holdings.begin(), this->holdings.end(), 0,
		[](double sum, const auto& kv) {
			return sum + kv.second.volume;
		});
}

// ---- Active Order Operations ----
void Agent::upsertActiveOrder(std::shared_ptr<Order> order) {
	switch (order->side) {
	case OrderAction::BID:
		this->activeBids.insert_or_assign(order->id, std::move(order));
		break;
	case OrderAction::ASK:
		this->activeAsks.insert_or_assign(order->id, std::move(order));
		break;
	}
}
void Agent::removeActiveOrder(std::shared_ptr<Order> order) {
	bool orderRemoved = false;

	switch (order->side) {
	case OrderAction::BID:
		orderRemoved = this->activeBids.erase(order->id);
		break;
	case OrderAction::ASK:
		orderRemoved = this->activeAsks.erase(order->id);
		break;
	}

	if (orderRemoved) { /* Order was removed */ }
	else { /* Order ID does not exist */ }
}

// ---- Action Operations ----
void Agent::act() {

}
OrderAction Agent::getAction() {

}
std::shared_ptr<Order> Agent::makeMarketBid() {

}
std::shared_ptr<Order> Agent::makeLimitBid() {

}
std::shared_ptr<Order> Agent::makeMarketAsk() {

}
std::shared_ptr<Order> Agent::makeLimitAsk() {

}
void Agent::cancelOrder() {

}
void Agent::hold() {

}

// ---- Utility Operations ----
void Agent::resetToInitial(double initialCash = 100.00) {

}
double Agent::getBetaPrice(double currentPrice, OrderAction side, double a = 2.0, double b = 5.0, double epsilon) {

}
double Agent::getMaxVariance(double price, double scale = 0.10, double decayRate = 0.25, double amplitude = 0.10, double frequency = PI * 2) {

}