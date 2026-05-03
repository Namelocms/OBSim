#include "include/MatchingEngine.h"
#include "include/Order.h"
#include "include/OrderBook.h"
#include "include/Agent.h"
#include "include/Enums.h"
#include "include/Util.h"
#include "include/SimClock.h"

MatchingEngine::MatchingEngine(OrderBook& ob) : OB(ob) {}

// ---- BID Operations ----

void MatchingEngine::matchMarketBid(std::shared_ptr<Order> order) {
	std::shared_ptr<Agent> biddingAgent = this->OB.agents[order->agentId];
	std::shared_ptr<Agent> askingAgent;
	std::shared_ptr<Order> bestAsk;
	int tradeVol = 0;
	int affordableVol = 0;
	int totalVolume = 0;
	double tradeCost = 0.00;
	double totalCost = 0.00;

	auto it = this->OB.askQueue.begin();
	while (it != this->OB.askQueue.end() && order->volume > 0) {
		bestAsk = *it;
		askingAgent = this->OB.agents[bestAsk->agentId];

		// Clean stagnant canceled orders
		if (bestAsk->status == OrderStatus::CANCELED) {
			it = this->OB.askQueue.erase(it);
			continue;
		}

		// Cancel new order when bidder id is the same as asker id
		if (askingAgent->id == biddingAgent->id) {
			order->status = OrderStatus::CANCELED;
			break;
		}

		// Find volume of shares to trade
		affordableVol = this->getAffordableVolume(bestAsk->price, biddingAgent->cash);
		tradeVol = std::min({ order->volume, bestAsk->volume, affordableVol });
		if (tradeVol < 1) { break; }
		
		// Calculate cost of trade
		tradeCost = roundTo(tradeVol * bestAsk->price);
		totalCost += tradeCost;

		// Update the asking agent's cash
		askingAgent->updateCash(tradeCost);
		
		// Update the bidding agent's cash and holdings
		biddingAgent->updateCash(-tradeCost);
		biddingAgent->upsertHolding(Holding(bestAsk->price, tradeVol));

		this->OB.updateCurrentPrice(bestAsk->price);

		order->volume -= tradeVol;

		this->OB.fillOrder(bestAsk, tradeVol);
		if (bestAsk->volume == 0) {
			it = this->OB.askQueue.erase(it);
		}
		else { break; }
	}

	order->status = (order->volume > 0) ? OrderStatus::CANCELED : OrderStatus::CLOSED;

	// Handle tick counting if bid order was filled
	if (order->volume == 0) {
		OB.tickCount++;
		OB.tickHistory.push_back(PriceTime(OB.currentPrice, OB.clock->simTimeMs));
	}

	// Volume Weighted Average Price (VWAP) of shares bought for order
	totalVolume = order->entryVolume - order->volume;
	if (totalVolume > 0) {
		order->price = totalCost / totalVolume;
	}
}
void MatchingEngine::matchLimitBid(std::shared_ptr<Order> order) {
	std::shared_ptr<Agent> biddingAgent = this->OB.agents[order->agentId];
	std::shared_ptr<Agent> askingAgent;
	std::shared_ptr<Order> bestAsk;
	int tradeVol = 0;
	int totalVolume = 0;
	double tradeCost = 0.00;
	double refund = 0.00;

	auto it = this->OB.askQueue.begin();
	while (it != this->OB.askQueue.end() && order->volume > 0) {
		bestAsk = *it;
		askingAgent = this->OB.agents[bestAsk->agentId];

		// Prevent trading with higher priced ask limit orders
		if (bestAsk->price > order->price) { break; }

		// Clean stagnant canceled orders
		if (bestAsk->status == OrderStatus::CANCELED) {
			it = this->OB.askQueue.erase(it);
			continue;
		}

		// Cancel new order when bidder id is the same as asker id
		if (askingAgent->id == biddingAgent->id) {
			order->status = OrderStatus::CANCELED;
			break;
		}

		tradeVol = std::min({ order->volume, bestAsk->volume });
		if (tradeVol < 1) { break; }

		tradeCost = roundTo(tradeVol * bestAsk->price);

		// Refund the buyer for price improvement
		refund = roundTo(tradeVol * (order->price - bestAsk->price));
		if (refund > 0) {
			biddingAgent->updateCash(refund);
		}

		askingAgent->updateCash(tradeCost);

		biddingAgent->upsertHolding(Holding(bestAsk->price, tradeVol));

		this->OB.fillOrder(bestAsk, tradeVol);
		this->OB.updateCurrentPrice(bestAsk->price);

		order->volume -= tradeVol;

		if (bestAsk->volume == 0) {
			it = this->OB.askQueue.erase(it);
		}
		else { break; }
	}

	if (order->volume > 0) {
		biddingAgent->upsertActiveOrder(order);
		this->OB.addOrder(order);
	}
	else {
		order->status = OrderStatus::CLOSED;

		// Handle tick counting if bid order was filled
		OB.tickCount++;
		OB.tickHistory.push_back(PriceTime(OB.currentPrice, OB.clock->simTimeMs));
	}
}

// ---- ASK Operations ----

void MatchingEngine::matchMarketAsk(std::shared_ptr<Order> order) {
	std::shared_ptr<Agent> askingAgent = this->OB.agents[order->agentId];
	std::shared_ptr<Agent> biddingAgent;
	std::shared_ptr<Order> bestBid;
	int tradeVol = 0;
	int totalVolume = 0;
	double tradeCost = 0.00;
	double totalCost = 0.00;

	auto it = this->OB.bidQueue.begin();
	while (it != this->OB.bidQueue.end() && order->volume > 0) {
		bestBid = *it;
		biddingAgent = this->OB.agents[bestBid->agentId];

		// Clean stagnant canceled orders
		if (bestBid->status == OrderStatus::CANCELED) {
			it = this->OB.bidQueue.erase(it);
			continue;
		}

		// Cancel new order when asker id is the same as bidder id
		if (biddingAgent->id == askingAgent->id) {
			order->status = OrderStatus::CANCELED;
			break;
		}

		tradeVol = std::min({ order->volume, bestBid->volume });
		if (tradeVol < 1) { break; }

		// Calculate cost of trade
		tradeCost = roundTo(tradeVol * bestBid->price);
		totalCost += tradeCost;

		// Update the asking agent's cash
		askingAgent->updateCash(tradeCost);

		// Update the bidding agent's holdings
		biddingAgent->upsertHolding(Holding(bestBid->price, tradeVol));

		this->OB.updateCurrentPrice(bestBid->price);

		order->volume -= tradeVol;

		this->OB.fillOrder(bestBid, tradeVol);
		if (bestBid->volume == 0) {
			it = this->OB.bidQueue.erase(it);
		}
		else { break; }
	}

	if (order->volume > 0) {
		order->status = OrderStatus::CANCELED;
		auto returnableShares = order->getReturnableShares();
		for (Holding h : returnableShares) {
			askingAgent->upsertHolding(h);
		}
	}
	else {
		order->status = OrderStatus::CLOSED;

		// Handle tick counting if bid order was filled
		OB.tickCount++;
		OB.tickHistory.push_back(PriceTime(OB.currentPrice, OB.clock->simTimeMs));
	}

	// Volume Weighted Average Price (VWAP) of shares bought for order
	totalVolume = order->entryVolume - order->volume;
	if (totalVolume > 0) {
		order->price = totalCost / totalVolume;
	}
}
void MatchingEngine::matchLimitAsk(std::shared_ptr<Order> order) {
	std::shared_ptr<Agent> askingAgent = this->OB.agents[order->agentId];
	std::shared_ptr<Agent> biddingAgent;
	std::shared_ptr<Order> bestBid;
	int tradeVol = 0;
	int totalVolume = 0;
	double tradeCost = 0.00;

	auto it = this->OB.bidQueue.begin();
	while (it != this->OB.bidQueue.end() && order->volume > 0) {
		bestBid = *it;
		biddingAgent = this->OB.agents[bestBid->agentId];

		// Prevent trading with higher priced ask limit orders
		if (bestBid->price < order->price) { break; }

		// Clean stagnant canceled orders
		if (bestBid->status == OrderStatus::CANCELED) {
			it = this->OB.bidQueue.erase(it);
			continue;
		}

		// Cancel new order when bidder id is the same as asker id
		if (askingAgent->id == biddingAgent->id) {
			order->status = OrderStatus::CANCELED;
			break;
		}

		tradeVol = std::min({ order->volume, bestBid->volume });
		if (tradeVol < 1) { break; }

		tradeCost = roundTo(tradeVol * bestBid->price);

		askingAgent->updateCash(tradeCost);

		biddingAgent->upsertHolding(Holding(bestBid->price, tradeVol));

		this->OB.fillOrder(bestBid, tradeVol);
		this->OB.updateCurrentPrice(bestBid->price);

		order->volume -= tradeVol;

		if (bestBid->volume == 0) {
			it = this->OB.bidQueue.erase(it);
		}
		else { break; }
	}

	if (order->volume > 0) {
		askingAgent->upsertActiveOrder(order);
		this->OB.addOrder(order);
	}
	else {
		order->status = OrderStatus::CLOSED;

		// Handle tick counting if bid order was filled
		OB.tickCount++;
		OB.tickHistory.push_back(PriceTime(OB.currentPrice, OB.clock->simTimeMs));
	}
}

// ---- Utility Operations ----

int MatchingEngine::getAffordableVolume(double targetPrice, double actingAgentCash) {
	return int(actingAgentCash / targetPrice);
}