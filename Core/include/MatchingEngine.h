#pragma once
#include <memory>
#include "Enums.h"

class OrderBook;
class Order;
class Agent;
enum class PrintKind;
enum class TimeInForce;

/* ---- Auctions (OrderModelPlan Step 3.1) ---- */

/* How far above the last price a market-on-open or market-on-close BUY is escrowed, and so the
*  most it will pay. A cash account cannot be allowed to spend without a bound; real brokers
*  check an on-close buy against an estimate in the same way. A starting value. */
inline constexpr double AUCTION_MARKET_COLLAR = 0.10;
/* Minutes before the regular close after which on-close orders are no longer accepted, as on
*  the major venues (15:50) */
inline constexpr double CLOSE_ORDER_CUTOFF_MINUTES = 10.0;

/* What a cross would do if run now: its price, the volume it would match, and what is left
*  over on the heavier side */
struct CrossResult {
	double price = 0.0;
	unsigned long long matched = 0;
	unsigned long long buyVolume = 0;
	unsigned long long sellVolume = 0;
	/* BID if more is bid than offered at the price, ASK if the other way */
	OrderAction imbalanceSide = OrderAction::BID;
	unsigned long long imbalance = 0;
};

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
	/* Place a midpoint-pegged order: trade it against opposite pegs at the midpoint if it can,
	*  then rest what is left in the midpoint book (OrderModelPlan Step 3.4) */
	void matchPeg(std::shared_ptr<Order> order);

	/* Work out a cross over the given auction orders and the eligible continuous book, without
	*  changing anything. Price maximises matched volume, then minimises the imbalance, then
	*  sits nearest the reference, then is the lower -- how the Nasdaq Cross is specified. */
	CrossResult indicativeCross(TimeInForce which, double reference) const;
	/* Run a cross: match at one price, print once, and cancel whatever on-open or on-close
	*  orders it did not fill. Continuous orders it part filled stay resting. */
	CrossResult runCross(TimeInForce which, PrintKind kind, double reference);

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
	void chargeFees(Order& order, Agent& agent, bool isMaker, double price, unsigned int volume, bool auction = false);
	/* Everything every fill does after cash and shares have moved: group events, fees, the
	*  position baseline, margin re-checks and lending supply. One place, for both continuous
	*  legs and auction legs. */
	void afterFill(Order& taker, Agent& takerAgent, Order& maker, Agent& makerAgent, double price,
		unsigned int volume, bool auction);
	/* Trade an incoming order against opposite midpoint pegs, at the midpoint, before it walks
	*  the price-time book: the midpoint improves on the touch, so pegs there go first */
	void matchAgainstPegs(const std::shared_ptr<Order>& incoming, const std::shared_ptr<Agent>& agent);
	/* One leg against a resting peg, at the midpoint */
	void settlePegLeg(const std::shared_ptr<Order>& incoming, const std::shared_ptr<Agent>& incomingAgent,
		const std::shared_ptr<Order>& peg, const std::shared_ptr<Agent>& pegAgent, double mid, unsigned int volume);
	/* One matched pair in a cross, at the cross price */
	void settleAuctionLeg(Order& bid, Agent& bidAgent, Order& ask, Agent& askAgent, double price, unsigned int volume);
	/* Hand bought shares to the buyer: they cover its short first, fails before borrowed
	*  shares, and only what is left becomes a holding (OrderModelPlan Step 1.4) */
	void deliverShares(Agent& buyer, double price, unsigned int volume);
	/* The seller of a short sale now owes the shares: a located short becomes a loan, an
	*  exempt one a fail until it is borrowed or bought back */
	void openShort(const Order& sale, Agent& seller, unsigned int volume);
	/* Walk one side of the book for an incoming order, returning the total cost traded */
	template <typename Queue>
	double sweep(const std::shared_ptr<Order>& order, const std::shared_ptr<Agent>& agent, Queue& opposite);
	template <typename Queue>
	unsigned int fillableFrom(const std::shared_ptr<Order>& order, const Queue& opposite) const;
};
