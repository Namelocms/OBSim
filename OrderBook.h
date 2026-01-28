#pragma once
#include <unordered_map>
#include <memory>
#include <cmath>
#include <numeric>
#include <chrono>
#include <queue>
#include "Holding.h"
#include "Order.h" // for order queues

class Agent;
enum class OrderAction;
enum class OrderType;
enum class ID_TYPE;
enum class Session;

struct PriceTime {
	double price;
	std::time_t time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
};
struct Snapshot {
	Snapshot() = default;

};
struct CompareBid {
public:
	bool operator()(const std::shared_ptr<Order>& o1, const std::shared_ptr<Order>& o2) const {
		if (o1->price == o2->price) {
			return o1->timestamp > o2->timestamp; // earlier timestamp first
		}
		return o1->price < o2->price; // higher price first
	}
};
struct CompareAsk {
public:
	bool operator()(const std::shared_ptr<Order>& o1, const std::shared_ptr<Order>& o2) const {
		if (o1->price == o2->price) {
			return o1->timestamp > o2->timestamp; // earlier timestamp first
		}
		return o1->price > o2->price; // lower price first
	}
};

class OrderBook {
public:
	/* Tick price precision decimal points. Above $1 == 0.01, Below $1 == 0.0001 */
	double tickPrecision;
	/* Current session of the market */
	Session session;
	/* Last traded price */
	double currentPrice;
	/* Number of shares available to trade */
	int shareFloat;
	/* Log of price movements and their times */
	std::vector<PriceTime> tickHistory;
	/* Priority queue for bid limit orders */
	std::priority_queue<std::shared_ptr<Order>, std::vector<std::shared_ptr<Order>>, CompareBid> bidQueue;
	/* Priority queue for ask limit orders */
	std::priority_queue<std::shared_ptr<Order>, std::vector<std::shared_ptr<Order>>, CompareAsk> askQueue;
	/* Log of all orders placed by agents || OrderId: Order */
	std::unordered_map<std::string, std::shared_ptr<Order>> orderHistory;
	/* Log of all agents in the sim || AgentId: Agent */
	std::unordered_map<std::string, std::shared_ptr<Agent>> agents;

	OrderBook() = default;
	OrderBook(double currentPrice);

// ---- Agent Operations ----
	/* Update or insert an agent to the agents map */
	void upsertAgent(std::shared_ptr<Agent> agent);

// ---- Order Operations ----
	/* Get the best active bid/ask and remove it from the queue */
	std::shared_ptr<Order> getBest(OrderAction side);
	/* Get the best n active bid/ask without removing it from the queue */
	std::vector<std::shared_ptr<Order>> peekBestN(OrderAction side, int n = 1);
	/* Add a new order to the bid/ask queue and orderbook */
	void addOrder(std::shared_ptr<Order> order);
	/* Remove an order from the bid/ask queue, update the status to canceled, return assets if applicable */
	void cancelOrder(std::shared_ptr<Order> order, std::shared_ptr<Agent> agent);
	/* Order was filled, remove from queue, update status to CLOSED */
	void fillOrder(std::shared_ptr<Order> order);
	/* Order was partially filled, update volume, re-add it to queue */
	void partialFillOrder(std::shared_ptr<Order> order, int volFilled);

// ---- Utility Operations ----
	/* Make a unique id for an Agent or Order */
	std::string makeId(ID_TYPE type);
	/* Get a snapshot of the current state of the Order Book */
	Snapshot getSnapshot(int depth = 10);
	/* Get current tick count from given start index in tickHistory */
	int getTick(int startTick = 0);
	/* Resets the LOB to its initial state */
	void resetToInitial(double initialPrice, bool clearAgents = false);

private:
	/* Number of asks in the askQueue */
	int numAsks = 0;
	/* Number of bids in the bidQueue */
	int numBids = 0;
	/* Next number for order id generation */
	int nextOrderId = 1;
	/* Next number for agent id generation */
	int nextAgentId = 1;
	/* Maximum digits allowed in an id */
	int MAX_ID_DIGITS = 12;
	/* Symbol of the current stock (random) */
	std::string TICKER_SYMBOL = "NONE";

// ---- Order Operations ----
	/* Add an order into the bid/ask queue */
	void addToQueue(std::shared_ptr<Order> order);

// ---- Utility Operations ----
	/* Creates a random ticker symbol */
	std::string makeTickerSymbol();
	/* Return an agent's assets to them after an order is canceled */
	void returnAssets(std::shared_ptr<Order> order, std::shared_ptr<Agent> agent);
	/* Clear the specified side's order queue */
	void clearQueues();
	/* Set the decimal precision to use based on current price */
	double setTickPrecision(double price);
};

