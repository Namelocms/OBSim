#pragma once
#include <memory>

class OrderBook;
class Order;

class MatchingEngine {
public:
	/* OrderBook */
	OrderBook& OB;

	MatchingEngine() = default;
	MatchingEngine(OrderBook& ob);

// ---- BID Operations ----
	/* Match a market bid order against open limit ask orders */
	void matchMarketBid(std::shared_ptr<Order> order);
	/* Match a limit bid order against open limit ask orders */
	void matchLimitBid(std::shared_ptr<Order> order);

// ---- ASK Operations ----
	/* Match a market ask order against open limit bid orders */
	void matchMarketAsk(std::shared_ptr<Order> order);
	/* Match a limit ask order against open limit bid orders */
	void matchLimitAsk(std::shared_ptr<Order> order);

private:
	/* Get the max amount of shares an agent can afford at the given targetPrice */
	unsigned int getAffordableVolume(double targetPrice, double actingAgentCash);
};

