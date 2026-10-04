#pragma once
#include <memory>
#include "Enums.h" // TimeInForce and SessionMask, needed complete for OrderRequest's defaults

class OrderBook;
class MatchingEngine;
class Order;
class Agent;

/* Longest a GTC order lives, in sim days
*
* Real brokers cap good-till-cancelled orders, typically somewhere from 60 to 180 days, so a
* forgotten order cannot sit in the book forever. 90 is a starting point in that range and
* sits in the order model plan's calibration register.
*/
inline constexpr int GTC_MAX_DAYS = 90;

/* Who sent an order. Recorded on the request so a log or a check can tell an agent's own
*  decision apart from one made on its behalf. */
enum class OrderOrigin {
	/* The agent's own decision */
	AGENT,
	/* The user, through a frontend (OrderModelPlan Step 4.2) */
	USER,
	/* The broker closing an account out on a margin call (Step 1.2) */
	LIQUIDATION,
	/* A held order that triggered: a stop, a trailing stop, a contingent leg (Phase 2) */
	TRIGGER,
};

/* Everything the broker needs to place an order, and nothing it should work out for itself
*
* An agent decides side, type, price, size and lifetime -- that is where its randomness
* lives. Turning that into an Order with an id, escrowing it and routing it is the broker's
* job, so it happens in one place for agents, the user and the broker's own liquidations.
*/
struct OrderRequest {
	OrderAction side;
	OrderType type;
	/* Limit price, ignored for a market order */
	double price = -1.0;
	unsigned int volume = 0;
	/* Sim time a GTD order expires at, see Agent::rollOrderExpiry. Worked out by the broker
	*  for DAY and GTC, and meaningless for IOC and FOK, which never rest. */
	double expiresAtMs = 0.0;
	/* GTD by default, because that is what every agent order has always been: a limit order
	*  rolled to a session boundary and cancelled there. A market order is always IOC. */
	TimeInForce tif = TimeInForce::GTD;
	/* Sessions the order may trade in. All of them by default, which is how the book behaved
	*  before orders carried a mask at all. */
	SessionMask sessions = SESSIONS_ALL;
	OrderOrigin origin = OrderOrigin::AGENT;

	/* An empty request is how a decision says "nothing to place" */
	bool empty() const { return this->volume == 0; }
};

/* What the broker has been asked to do over a run
*
* Measurement only: nothing reads these to make a decision, so they cannot change what the
* market does. Exists so cancel/replace adoption (and later fees, margin, triggers) can be
* reported as numbers rather than asserted. Reset at the start of every run.
*/
struct BrokerStats {
	long long submitted = 0;
	long long refused = 0;
	long long cancels = 0;
	long long replaces = 0;
	/* Replaces that kept time priority (a size decrease) */
	long long replacesInPlace = 0;
	/* Replaces refused because the account could not carry the new order */
	long long replacesRefused = 0;
	/* Sum and count of how long an order had rested when it was cancelled or replaced, ms */
	double restedMsSum = 0.0;
	long long restedSamples = 0;
	/* Sum and count of the spread seen as each order arrived, when both sides were quoted */
	double spreadSum = 0.0;
	long long spreadSamples = 0;
};

/* ---- Broker ----
*
* Sits between whoever decides to trade and the exchange (OrderBook + MatchingEngine), as a
* real broker does. Owns the account side of an order: checking the account can carry it,
* reserving its escrow, and -- as OrderModelPlan Phase 1 and 2 land -- commissions, margin,
* locates, held orders and liquidation. The exchange owns the book and the matching.
*
* Stateless today apart from its references, so it adds nothing to a run's state and is
* behaviour-neutral by construction.
*/
class Broker {
public:
	OrderBook& OB;
	MatchingEngine& ME;
	BrokerStats stats;

	Broker(OrderBook& ob, MatchingEngine& me);

	/* Validate, escrow and route an order for this agent, returning it after matching
	*
	* Returns nullptr, having changed nothing, when the request is empty, malformed (a FOK
	* market order, a GTD already past, no sessions) or the account cannot carry it. The
	* returned order may be CLOSED (filled), OPEN (resting) or CANCELED (an IOC or market
	* remainder, a FOK that could not fill, or killed by self trade protection).
	*/
	std::shared_ptr<Order> submit(const OrderRequest& request, const std::shared_ptr<Agent>& agent);
	/* Cancel a resting order on its owner's behalf, returning its escrow */
	void cancel(const std::shared_ptr<Order>& order, const std::shared_ptr<Agent>& agent);
	/* Change a resting limit order's price and/or size, under exchange priority rules
	*
	* A size DECREASE at the same price keeps time priority: the order shrinks in place and
	* the released escrow comes back. A PRICE CHANGE or a size INCREASE loses it: the order
	* leaves the queue, takes the current time as its timestamp, and re-enters exactly as a
	* new order would -- so a replace that has become marketable executes immediately, and
	* whatever is left rests at the back of its new price level.
	*
	* Keeps the order's id, time-in-force, expiry and sessions. Returns the order, or nullptr
	* having changed nothing if it is not this agent's resting limit order, the new terms are
	* malformed, or the account cannot carry them. A request that changes nothing returns the
	* order untouched.
	*/
	std::shared_ptr<Order> replace(const std::shared_ptr<Order>& order, const std::shared_ptr<Agent>& agent,
		double newPrice, unsigned int newVolume);

private:
	/* Everything submit does except the measurement around it */
	std::shared_ptr<Order> place(const OrderRequest& request, const std::shared_ptr<Agent>& agent);
	/* Kill an order that never reached the book, returning the escrow submit reserved for it.
	*  Uses the same refund arithmetic as OrderBook::cancelOrder so the two paths agree. */
	void killUnplaced(const std::shared_ptr<Order>& order, const std::shared_ptr<Agent>& agent);
};
