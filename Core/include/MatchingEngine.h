#pragma once
#include <memory>

class OrderBook;
class Order;
class Agent;

class MatchingEngine {
public:
	/* OrderBook */
	OrderBook& OB;

	MatchingEngine() = default;
	MatchingEngine(OrderBook& ob);

	/* Match an incoming order against the opposite side of the book
	*
	* The single entry point for every side and type. One loop walks the opposite queue in
	* price-time priority and every executed leg goes through settleLeg, so anything that
	* has to happen on a fill (fees, borrow, margin, triggers) has exactly one place to live.
	*
	* What differs between the four side/type combinations is confined to three places:
	* whether there is a limit price to stop at, what bounds the leg's volume, and what is
	* done with the remainder once the walk stops. See finishIncoming.
	*/
	void match(std::shared_ptr<Order> order);
	/* How much of this order would fill if it arrived now, without changing anything
	*
	* Walks the opposite side exactly as match would -- same price limit, same session
	* eligibility, stopping where self trade protection would kill the order -- but only
	* adds up volume. What fill-or-kill decides on. Capped at the order's own volume.
	*/
	unsigned int fillableVolume(const std::shared_ptr<Order>& order) const;

private:
	/* Settle one executed leg between the incoming order and a resting one
	*
	* The ONLY place cash and shares move for a fill. Trades at the resting order's price.
	* Returns the leg's cost so a market order can report its VWAP.
	*/
	double settleLeg(const std::shared_ptr<Order>& incoming, const std::shared_ptr<Agent>& incomingAgent,
		const std::shared_ptr<Order>& resting, const std::shared_ptr<Agent>& restingAgent, unsigned int volume);
	/* Decide what becomes of the incoming order once the walk has stopped
	*
	* Limit orders rest whatever is left. Market orders never rest, so their remainder is
	* cancelled and its escrow (reserved shares, for an ask) returned. An order that self
	* trade protection killed keeps none of its remainder, and its escrow is returned.
	*/
	void finishIncoming(const std::shared_ptr<Order>& order, const std::shared_ptr<Agent>& agent, double totalCost);
	/* Charge one side of a leg its fees (OrderModelPlan Step 1.1)
	*
	* Accrues commission, maker-taker and regulatory fees at full precision against the
	* order, charges the agent the increase in their rounded total, takes it from the order's
	* fee reserve first if it has one, and books every part to the ledger. Does nothing with
	* fees off.
	*/
	void chargeFees(Order& order, Agent& agent, bool isMaker, double price, unsigned int volume);
	/* Walk one side of the book for an incoming order, returning the total cost traded */
	template <typename Queue>
	double sweep(const std::shared_ptr<Order>& order, const std::shared_ptr<Agent>& agent, Queue& opposite);
	template <typename Queue>
	unsigned int fillableFrom(const std::shared_ptr<Order>& order, const Queue& opposite) const;
};
