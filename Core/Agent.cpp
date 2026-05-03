#include "include/Agent.h"
#include "include/Enums.h"
#include "include/Util.h"
#include "include/Order.h"
#include "include/Holding.h"
#include "include/OrderBook.h"
#include "include/MatchingEngine.h"

Agent::Agent(std::string id, double reactionTime, double cash, AgentStatus status, OrderBook& ob, MatchingEngine& me) :
	id(id), reactionTime(reactionTime), cash(cash), status(status), OB(ob), ME(me) { }

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

void Agent::actRandom() {
	OrderAction action = this->getRandomAction();
	OrderType orderType = randomInt(0, 1) ? OrderType::MARKET : OrderType::LIMIT;
	std::shared_ptr<Order> order;

	switch (action) {
	case OrderAction::BID:
		switch (orderType) {
		case OrderType::MARKET:
			order = this->makeMarketBid();
			this->ME.matchMarketBid(order);
			break;
		case OrderType::LIMIT:
			order = this->makeLimitBid();
			this->ME.matchLimitBid(order);
			break;
		}
		break;
	case OrderAction::ASK:
		switch (orderType) {
		case OrderType::MARKET:
			order = this->makeMarketAsk();
			this->ME.matchMarketAsk(order);
			break;
		case OrderType::LIMIT:
			order = this->makeLimitAsk();
			this->ME.matchLimitAsk(order);
			break;
		}
		break;
	case OrderAction::CANCEL:
		this->cancelOrder();
		break;
	case OrderAction::HOLD:
		this->hold();
		break;
	}
}
OrderAction Agent::getRandomAction() {
	std::vector<OrderAction> availableActions = { OrderAction::HOLD };
	int totalHoldings = this->getTotalHoldings();
	int actionChoice = 0;
	int numActions = 1;

	if (this->cash >= this->OB.currentPrice) { availableActions.push_back(OrderAction::BID); }
	if (totalHoldings > 0) { availableActions.push_back(OrderAction::ASK); }
	if (!this->activeAsks.empty() || !this->activeBids.empty()) { availableActions.push_back(OrderAction::CANCEL); }

	numActions = availableActions.size();
	if (numActions == 1) { actionChoice = 0; }
	else { actionChoice = randomInt(0, numActions - 1); }

	return availableActions[actionChoice];
}
std::shared_ptr<Order> Agent::makeMarketBid() {
	int maxPurchasable = int(this->cash / this->OB.currentPrice);
	if (maxPurchasable < 1) { return nullptr; }

	int chosenVol = randomInt(1, maxPurchasable);

	std::shared_ptr<Order> order = std::make_shared<Order>(
		this->OB.makeId(ID_TYPE::ORDER),
		this->id,
		-1,
		chosenVol,
		OrderAction::BID,
		OrderType::MARKET
	);

	return order;
}
std::shared_ptr<Order> Agent::makeLimitBid() {
	double chosenPrice = this->getBetaPrice(this->OB.currentPrice, OrderAction::BID);
	int maxPurchasable = int(this->cash / chosenPrice);
	if (maxPurchasable < 1) { return nullptr; }

	int chosenVol = randomInt(1, maxPurchasable);
	double totalValue = roundTo(chosenPrice * chosenVol);

	std::shared_ptr<Order> order = std::make_shared<Order>(
		this->OB.makeId(ID_TYPE::ORDER),
		this->id,
		chosenPrice,
		chosenVol,
		OrderAction::BID,
		OrderType::LIMIT
	);

	this->updateCash(-totalValue);

	return order;
}
std::shared_ptr<Order> Agent::makeMarketAsk() {
	int chosenVol = 1;
	int totalHoldings = this->getTotalHoldings();
	if (totalHoldings < 1) { return nullptr; }
	if (totalHoldings > 1) { chosenVol = randomInt(1, totalHoldings); }

	std::vector<Holding> reservedHoldings = this->removeHoldings(chosenVol);

	std::shared_ptr<Order> order = std::make_shared<Order>(
		this->OB.makeId(ID_TYPE::ORDER),
		this->id,
		-1,
		chosenVol,
		OrderAction::ASK,
		OrderType::MARKET,
		reservedHoldings
	);

	return order;
}
std::shared_ptr<Order> Agent::makeLimitAsk() {
	double chosenPrice = this->getBetaPrice(this->OB.currentPrice, OrderAction::ASK);
	int chosenVol = 1;

	int totalHoldings = this->getTotalHoldings();
	if (totalHoldings < 1) { return nullptr; }
	if (totalHoldings > 1) { chosenVol = randomInt(1, totalHoldings); }

	std::vector<Holding> reservedHoldings = this->removeHoldings(chosenVol);

	std::shared_ptr<Order> order = std::make_shared<Order>(
		this->OB.makeId(ID_TYPE::ORDER),
		this->id,
		chosenPrice,
		chosenVol,
		OrderAction::ASK,
		OrderType::LIMIT,
		reservedHoldings
	);

	return order;
}
void Agent::cancelOrder() {
	bool hasBids = !activeBids.empty();
	bool hasAsks = !activeAsks.empty();

	if (!hasBids && !hasAsks) { return; }
	
	bool side = (hasBids && hasAsks) ? randomInt(0, 1) : (hasBids ? 0 : 1);

	if (side) {
		auto it = activeAsks.begin();
		std::advance(it, randomInt(0, static_cast<int>(activeAsks.size() - 1)));
		this->OB.cancelOrder(it->second, shared_from_this());
	}
	else {
		auto it = activeBids.begin();
		std::advance(it, randomInt(0, static_cast<int>(activeBids.size() - 1)));
		this->OB.cancelOrder(it->second, shared_from_this());
	}
}
void Agent::hold() {
	return;
}

// ---- Utility Operations ----

void Agent::resetToInitial(double initialCash) {
	this->cash = initialCash;
	this->status = AgentStatus::INACTIVE;
	this->holdings.clear();
	this->activeAsks.clear();
	this->activeBids.clear();
}
double Agent::getBetaPrice(double currentPrice, OrderAction side, double a, double b, double epsilon) {
	double x, discount, premium, preRounded, precision, betaPrice;
	double maxVariance = this->getMaxVariance(currentPrice);

	switch (side) {
	case OrderAction::BID:
		x = sampleBeta(a, b);
		discount = x * maxVariance;
		preRounded = std::max(currentPrice * (1 - discount), epsilon);
		precision = (preRounded < 1.00) ? 0.0001 : 0.01;
		betaPrice = roundTo(preRounded, precision);
		return betaPrice;
	case OrderAction::ASK:
		x = sampleBeta(a, b);
		premium = x * maxVariance;
		preRounded = currentPrice * (1 + premium);
		precision = (preRounded < 1.00) ? 0.0001 : 0.01;
		betaPrice = roundTo(preRounded, precision);
		return betaPrice;
	}
	return roundTo(currentPrice, this->OB.tickPrecision);
}
double Agent::getMaxVariance(double price, double scale, double decayRate, double amplitude, double frequency) {
	return scale * (pow(price, -decayRate)) * (1 + (amplitude * sin(frequency * log(price))));
}