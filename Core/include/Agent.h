#pragma once
#include <unordered_map>
#include <string>
#include <memory>
#include <cmath>
#include <numeric>

enum class OrderAction;
enum class AgentStatus;
class OrderBook;
class MatchingEngine;
class Order;
class Holding;

const double PI = 3.14159265358979323846;

class Agent : public std::enable_shared_from_this<Agent> {
public:
	/* Agent's unique ID */
	const std::string id;
	/* Agent's minimum reaction time in milliseconds */
	double reactionTime;
	/* Agent's buying power */
	double cash;
	/* Agent's status */
	AgentStatus status;
	/* All shares the agent currently holds */
	std::unordered_map<double, Holding> holdings;
	/* All open ask orders placed by the agent */
	std::unordered_map<std::string, std::shared_ptr<Order>> activeAsks;
	/* All open bid orders placed by the agent */
	std::unordered_map<std::string, std::shared_ptr<Order>> activeBids;
	/* The orderbook for the current stock */
	OrderBook& OB;
	/* The matching engine */
	MatchingEngine& ME;

	Agent() = default;
	Agent(
		std::string id,
		double reactionTime,
		double cash,
		AgentStatus status,
		OrderBook& ob,
		MatchingEngine& me
	);

// ---- Cash Operations ----
	/* Update the cash holdings of this agent [Negative amt decreses cash] */
	void updateCash(double amt);

// ---- Holdings Operations ----
	/* Update/insert a share in the agent's holdings */
	void upsertHolding(Holding holding);
	/* Remove the given volume of holdings, return the list of holdings objects of the removed shares */
	std::vector<Holding> removeHoldings(int volume);
	/* Get the number of shares the Agent is currently holding */
	int getTotalHoldings();

// ---- Active Order Operations ----
	/* Update/insert an active order */
	void upsertActiveOrder(std::shared_ptr<Order> order);
	/* Remove an active order */
	void removeActiveOrder(std::shared_ptr<Order> order);

// ---- Action Operations ----
	/* Execute a chosen action */
	void actRandom();
	/* Choose a random OrderAction given the agent's current holdings and cash */
	OrderAction getRandomAction();
	/* Make a random market bid order */
	std::shared_ptr<Order> makeMarketBid();
	/* Make a random limit bid order */
	std::shared_ptr<Order> makeLimitBid();
	/* Make a random market ask order */
	std::shared_ptr<Order> makeMarketAsk();
	/* Make a random limit ask order */
	std::shared_ptr<Order> makeLimitAsk();
	/* Cancel a random order */
	void cancelOrder();
	/* Do nothing */
	void hold();

// ---- Utility Operations ----
	/* Resets the agent to its initial state */
	void resetToInitial(double initialCash = 100.00);
	/* Get random price within custom beta distribution
	*
	* Can be shaped to hug the current price without reaching it: [a > b: hugs lower end || a < b: hugs upper end].
	* Has a nice distribution just a little past/before the current price is where most orders will be placed.
	* a = Higher favors right side (larger x).
	* b = Higher favors left side (smaller x).
	* epsilon = Minimum possible price.
	*/
	double getBetaPrice(double currentPrice, OrderAction side, double a = 2.0, double b = 5.0, double epsilon = 0.0001);

private:
	/* Get the max variance in price (how much the computed price will differ from passed price)
	*
	* scale = Scale factor, controls the overall height of the curve
		decay_rate = The power-law decay rate, larger decay_rate -> variance falls off faster as price increases
		amplitude = The sinusoidal amplitude, controls how much the variance "wiggles" above/below the base curve. Set to 0 to disable sine behavior
		frequency = The frequency of the sine (in log space), higher frequency -> more wiggles per log unit of price

		PRICE__||  max_variance
		0.1____||  0.17783
		1______||  0.10000
		10_____||  0.05623
		100____||  0.03162
		1000___||  0.01778
		10000__||  0.01000
		100000_||  0.00562
	*/
	double getMaxVariance(double price, double scale = 0.10, double decayRate = 0.25, double amplitude = 0.10, double frequency = PI * 2);

};

