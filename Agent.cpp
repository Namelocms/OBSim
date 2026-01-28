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
	auto it = this->holdings.find(holding.price);
	if (it != this->holdings.end()) {
		this->holdings[holding.price].volume += holding.volume;
	}
	else {
		this->holdings[holding.price] = holding;
	}
}
std::vector<Holding> Agent::removeHoldings(int volume) {
	std::vector<Holding> removedHoldings;
	Holding currentHolding;

	while (volume > 0 && !this->holdings.empty()) {
		currentHolding = holdings.begin()->second;

		// Clean faulty holdings
		if (currentHolding.volume <= 0) {
			this->holdings.erase(currentHolding.price);
			continue;
		}

		if (currentHolding.volume <= volume) {
			// Fully remove holding
			this->holdings.erase(currentHolding.price);
			volume -= currentHolding.volume;
			removedHoldings.push_back(currentHolding);
		}
		else {
			this->holdings[currentHolding.price].volume -= volume;
			removedHoldings.push_back(Holding(currentHolding.price, volume));
			volume = 0;
		}
	}

	return removedHoldings;
}
int Agent::getTotalHoldings() {

}

// ---- Active Order Operations ----
void Agent::upsertActiveOrder(std::shared_ptr<Order> order) {

}
void Agent::removeActiveOrder(std::shared_ptr<Order> order) {

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