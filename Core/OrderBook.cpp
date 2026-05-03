#include "include/OrderBook.h"
#include "include/Enums.h"
#include "include/Agent.h"
#include "include/Util.h"
#include "include/SimClock.h"

OrderBook::OrderBook(double currentPrice, unsigned int shareFloat) : clock(clock), currentPrice(currentPrice), shareFloat(shareFloat) {
	this->setTickPrecision(currentPrice);
	this->tickCount = 0;
	this->shareFloat = (shareFloat == 0) ? randomInt(100'000, 100'000'000) : shareFloat;
	this->TICKER_SYMBOL = this->makeTickerSymbol();
	this->session = Session::PREMARKET;
}

// ---- Agent Operations ----

void OrderBook::upsertAgent(std::shared_ptr<Agent> agent) {
	this->agents.insert_or_assign(agent->id, agent);
}

// ---- Order Operations ----

std::shared_ptr<Order> OrderBook::getBest(OrderAction side) {
	return (side == OrderAction::BID) ? this->removeBestFrom(this->bidQueue) : this->removeBestFrom(this->askQueue);
}
std::vector<std::shared_ptr<Order>> OrderBook::peekBestN(OrderAction side, unsigned char n) {
	return (side == OrderAction::BID) ? this->peekBestFrom(this->bidQueue, n) : this->peekBestFrom(this->askQueue, n);
}
void OrderBook::addOrder(std::shared_ptr<Order> order) {
	if (order == nullptr) { return; }

	//this->orderHistory.emplace(order->id, order);
	if (order->side == OrderAction::BID) {
		this->bidQueue.insert(order);
		this->numBids++;
	}
	else {
		this->askQueue.insert(order);
		this->numAsks++;
	}
}
void OrderBook::cancelOrder(std::shared_ptr<Order> order, std::shared_ptr<Agent> agent) {
	if (order == nullptr) { return; }

	order->status = OrderStatus::CANCELED;
	if (order->side == OrderAction::BID) {
		agent->updateCash(order->price * order->volume);
		agent->removeActiveOrder(order);
		this->bidQueue.erase(order);
		this->numBids--;
	}
	else {
		std::vector<Holding> returnableHoldings = order->getReturnableShares();
		for (Holding h : returnableHoldings) {
			agent->upsertHolding(h);
		}
		agent->removeActiveOrder(order);
		this->askQueue.erase(order);
		this->numAsks--;
	}
}
void OrderBook::fillOrder(std::shared_ptr<Order> order, int volFilled) {
	if (order == nullptr) { return; }

	std::shared_ptr<Agent> agent = this->agents[order->agentId];

	order->volume -= volFilled;
	if (order->volume == 0) {
		order->status = OrderStatus::CLOSED;
		if (order->side == OrderAction::BID) {
			this->numBids--; 
		}
		else {
			this->numAsks--; 
		}
		agent->removeActiveOrder(order);
		this->tickCount++;
		this->tickHistory.push_back(PriceTime(this->currentPrice, this->clock->simTimeMs));
	}
}

// ---- Utility Operations ----

void OrderBook::updateCurrentPrice(double newPrice) {
	this->currentPrice = newPrice;
	this->setTickPrecision(this->currentPrice);
}
std::string OrderBook::makeId(ID_TYPE type) {
	std::string newId = "";
	int numChars = 0;
	int remainingChars = 0;

	switch (type) {
	case ID_TYPE::ORDER:
		newId += "O-";
		numChars = std::to_string(this->nextOrderId).size();
		remainingChars = this->MAX_ID_DIGITS - numChars;
		newId += std::string(remainingChars, '0') + std::to_string(this->nextOrderId);
		this->nextOrderId++;
		break;
	case ID_TYPE::AGENT:
		newId += "A-";
		numChars = std::to_string(this->nextAgentId).size();
		remainingChars = this->MAX_ID_DIGITS - numChars;
		newId += std::string(remainingChars, '0') + std::to_string(this->nextAgentId);
		this->nextAgentId++;
		break;
	}

	return newId;
}
Snapshot OrderBook::getSnapshot(unsigned char depth) {
	Snapshot snap = Snapshot();

	snap.currentPrice = this->currentPrice;
	snap.bids = this->peekBestN(OrderAction::BID, depth);
	snap.asks = this->peekBestN(OrderAction::ASK, depth);
	if (!snap.asks.empty() && !snap.bids.empty()) {
		snap.spread = snap.asks[0]->price - snap.bids[0]->price;
	}
	else {
		snap.spread = 0.0;
	}
	snap.macd = 0.00;// TODO
	snap.rsi = 0.00; // TODO
	snap.vwap = 0.00;// TODO
	snap.sma = 0.00; // TODO

	return snap;
}
int OrderBook::getTick(int startTick) {
	int totalTicks = this->tickHistory.size();
	return totalTicks - startTick;
}
void OrderBook::resetToInitial(double initialPrice, unsigned int shareFloat, bool clearAgents) {
	this->currentPrice = (initialPrice <= 0.00) ? -1.00 * (initialPrice - 0.01) : initialPrice;
	setTickPrecision(initialPrice);
	this->shareFloat = (shareFloat == 0) ? randomInt(100'000, 100'000'000) : shareFloat;
	this->tickHistory.clear();
	this->tickCount = 0;
	//this->orderHistory.clear();
	this->bidQueue.clear();
	this->askQueue.clear();
	this->numAsks = 0;
	this->numBids = 0;
	this->TICKER_SYMBOL = this->makeTickerSymbol();
	this->session = Session::PREMARKET;
	if (clearAgents) {
		this->nextOrderId = 1;
		this->nextAgentId = 1;
		this->agents.clear();
	}
}

// ---- Private Utility Operations ----

std::string OrderBook::makeTickerSymbol() {
	std::string symbol = "";

	for (int i = 0; i < 4; ++i) {
		symbol += (char)randomInt(65, 90);
	}

	return symbol;
}
void OrderBook::setTickPrecision(double price) {
	this->tickPrecision = (price < 1.00) ? 0.0001 : 0.01;
}