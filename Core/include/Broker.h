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

private:
	/* Kill an order that never reached the book, returning the escrow submit reserved for it.
	*  Uses the same refund arithmetic as OrderBook::cancelOrder so the two paths agree. */
	void killUnplaced(const std::shared_ptr<Order>& order, const std::shared_ptr<Agent>& agent);
};
