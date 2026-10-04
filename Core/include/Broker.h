#pragma once
#include <memory>
#include "Enums.h" // TimeInForce and SessionMask, needed complete for OrderRequest's defaults
#include "TriggerBook.h"
#include "OrderGroup.h"
#include <string>
#include <set>
#include <map>
#include <unordered_map>
#include <functional>

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
	/* How an ask is marked. The broker checks it: a LONG sale must be covered by holdings, a
	*  SHORT one by a locate, a SHORT_EXEMPT one by being a market maker. */
	SaleMark mark = SaleMark::LONG;
	/* A stop (OrderModelPlan Step 2.1): held by the broker until a print reaches stopPrice,
	*  then sent as this request's type -- a stop-market, or a stop-limit at price. */
	double stopPrice = 0.0;
	/* A trailing stop instead: the trigger follows the best price since placement at this
	*  distance, as an amount or a fraction. stopPrice is then worked out, not given. */
	double trailAmount = 0.0;
	double trailPercent = 0.0;
	bool isStop() const { return this->stopPrice > 0.0 || this->trailAmount > 0.0 || this->trailPercent > 0.0; }
	OrderOrigin origin = OrderOrigin::AGENT;

	/* An empty request is how a decision says "nothing to place" */
	bool empty() const { return this->volume == 0; }
};

/* An entry order with a take-profit and a stop-loss to be attached as it fills */
struct BracketRequest {
	OrderRequest entry;
	double takeProfit = 0.0;
	double stopLoss = 0.0;
	TimeInForce childTif = TimeInForce::GTC;
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

	// ---- Margin (OrderModelPlan Step 1.2) ----
	/* Margin calls met by liquidation, and liquidation orders sent */
	long long marginCalls = 0;
	long long liquidationOrders = 0;
	/* Accounts whose negative equity was written off */
	long long writeOffs = 0;
	/* Trigger pump rounds run, the most in one pump, and pumps stopped by MAX_TRIGGER_ROUNDS */
	long long triggerRounds = 0;
	int maxRoundsInOnePump = 0;
	long long roundCapHits = 0;
	/* Attempts to fire a trigger from inside matching. Must stay zero; harnesses assert it. */
	long long reentrancyBlocked = 0;
	/* Margin interest charged, in total, across all accounts, and the days it was accrued on */
	double interestCharged = 0.0;
	long long interestDays = 0;

	// ---- Lending (OrderModelPlan Step 1.3) ----
	/* Borrow fees charged to borrowers, paid to institutional lenders, and kept by the house */
	double borrowFeesCharged = 0.0;
	double borrowFeesToLenders = 0.0;
	long long borrowFeeDays = 0;

	// ---- Short selling (OrderModelPlan Step 1.4) ----
	/* Shares recalled because lenders sold them, buy-in orders sent, exempt fails borrowed at
	*  the close, and fails still outstanding at the open that had to be bought in */
	long long recalledShares = 0;
	long long buyInOrders = 0;
	long long failsBorrowed = 0;
	long long failsClosedOut = 0;

	// ---- Stops (OrderModelPlan Step 2.1) ----
	long long stopsPlaced = 0;
	long long stopsTriggered = 0;
	/* Triggered buy stops the account could no longer pay for, cancelled rather than placed */
	long long stopsRefusedAtTrigger = 0;
	long long stopsExpired = 0;
	long long stopsCancelled = 0;
	/* Trailing stop triggers moved by a new extreme */
	long long trailMoves = 0;

	// ---- Contingent orders (OrderModelPlan Step 2.2) ----
	long long groupsCreated = 0;
	long long groupsResolved = 0;
	/* OCO stop legs shrunk because the limit leg part filled */
	long long ocoReductions = 0;
	/* Bracket children grown because the entry filled some more */
	long long bracketGrowths = 0;
};

/* Most rounds one pump of the trigger book may run before it stops and says so
*
* Defense in depth, like the back-data caps: a cascade ends on its own once nothing more is
* crossed, so reaching this means something is wrong, and it is counted rather than hidden.
*/
inline constexpr int MAX_TRIGGER_ROUNDS = 64;

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
	/* Every price the broker is watching on its customers' behalf */
	TriggerBook triggers;
	/* Accounts that owe a buy-in, ordered so the pump visits them deterministically */
	std::set<std::string> buyIns;
	/* Margin calls are watched here, stops in their own book: whether a stop may fire depends
	*  on the session (Features::stops), a margin call's never does */
	TriggerBook stopTriggers;
	/* Every held order, by id */
	std::unordered_map<std::string, std::shared_ptr<Order>> held;
	/* Trailing stops by the extreme they have seen, so a print only touches the ones it moves:
	*  sells by their high, lowest first; buys by their low, highest first */
	std::multimap<double, std::string> trailingSells;
	std::multimap<double, std::string, std::greater<double>> trailingBuys;

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

	/* Act on everything the market's prints have triggered, until nothing more fires
	*
	* Called after every action that can trade -- an agent's, a liquidation's, later an
	* auction's -- and NEVER from inside matching. Each round:
	*
	*   1. re-checks the accounts whose cash or position changed, moving their margin trigger
	*      and catching any already in violation (interest can do that with no print at all)
	*   2. takes every trigger the prints since the last look have crossed, in price order
	*   3. liquidates the accounts in violation
	*
	* and the liquidations' own prints feed the next round, which is how a cascade happens.
	* Stops at MAX_TRIGGER_ROUNDS, counting it, and does nothing at all while matching is
	* under way.
	*/
	void processTriggers();
	/* Charge every margin debit a day of interest, on a 360 day year, and re-check the accounts */
	void accrueMarginInterest();
	/* Charge every stock loan a day of borrow fee at today's utilisation, pay institutional
	*  lenders their share pro rata, and book the rest -- the programme's cut and every retail
	*  rehypothecation fee -- to the house. Rebuilds the lendable supply first. */
	void accrueBorrowFees();
	/* Clear the triggers and stats for a new run */
	void reset();
	/* Cancel every held order whose time-in-force has run out, returning reserved shares */
	unsigned int expireHeld(double nowMs);
	/* May a stop trigger in the current session? */
	bool stopsActive() const;

	/* Place an OCO: a limit leg in the book and a stop leg held, same side, same size. The limit
	*  leg reserves the shares (or cash); the stop leg shares the reservation. */
	std::shared_ptr<OrderGroup> submitOco(const OrderRequest& limitLeg, const OrderRequest& stopLeg,
		const std::shared_ptr<Agent>& agent);
	/* Place a bracket: the entry now, its take-profit and stop-loss as it fills */
	std::shared_ptr<OrderGroup> submitBracket(const BracketRequest& request, const std::shared_ptr<Agent>& agent);
	/* The groups currently live, by id */
	std::unordered_map<std::string, std::shared_ptr<OrderGroup>> groups;
	/* At the regular close: borrow what can be borrowed against market makers' exempt shorts */
	void borrowForFails();
	/* At the regular open: any fail still outstanding must be bought in now (modelled on Reg SHO
	*  Rule 204's close-out, by the beginning of regular trading hours) */
	void closeOutFails();

private:
	/* Recompute one account's margin trigger, returning whether it is in violation right now */
	bool refreshMargin(const std::shared_ptr<Agent>& agent);
	/* Meet a margin call: cancel the account's orders, then sell enough to restore the target */
	void liquidate(const std::shared_ptr<Agent>& agent);
	/* Buy shares back to cover a short, cancelling its other bids first */
	void buyBack(const std::shared_ptr<Agent>& agent, unsigned int shares, OrderOrigin origin);
	/* Send the buy-in for a recalled loan or a fail that is due */
	void buyIn(const std::shared_ptr<Agent>& agent, unsigned int shares);
	/* Recall loans the pool can no longer cover, newest first */
	void recallLoans();
	/* Write off an account left with negative equity and nothing left to sell */
	void writeOffIfInsolvent(const std::shared_ptr<Agent>& agent);
	/* Act on the fills and cancels of group members recorded during matching */
	void processGroupEvents();
	/* A bracket's entry filled some more: create or grow its OCO pair by that much */
	void growBracket(OrderGroup& group, unsigned int volume);
	/* The pair is settled: mark it, and cancel what is left of a bracket's entry */
	void resolveGroup(OrderGroup& group);
	/* The OCO's held stop triggered: cancel the book leg, take over its shares, then release */
	void takeOverFromBookLeg(OrderGroup& group, const std::shared_ptr<Order>& heldLeg);
	/* Hold a stop leg that shares an OCO sibling's reservation instead of reserving its own */
	std::shared_ptr<Order> holdShared(OrderRequest request, const std::shared_ptr<Agent>& agent, const std::string& groupId);
	/* Take a stop or trailing stop into the broker's keeping, reserving a sell stop's shares */
	std::shared_ptr<Order> hold(const OrderRequest& request, const std::shared_ptr<Agent>& agent, bool reserveShares = true);
	/* Cancel a held order, returning what it reserved */
	void cancelHeld(const std::shared_ptr<Order>& order, const std::shared_ptr<Agent>& agent);
	/* A held order's trigger was reached: send it to the exchange as what its type says */
	void release(const std::shared_ptr<Order>& order);
	/* Move the trailing stops a print has set a new extreme for */
	void trailTo(double price);
	/* Where a trailing stop's trigger stands for a given extreme */
	static double trailingTrigger(const Order& order, double extreme);
	/* place() with the order tagged as a group member before it can trade */
	std::shared_ptr<Order> placeTagged(const OrderRequest& request, const std::shared_ptr<Agent>& agent, const std::string& groupId);
	/* Everything submit does except the measurement around it */
	std::shared_ptr<Order> place(const OrderRequest& request, const std::shared_ptr<Agent>& agent, const std::string& groupId = "");
	/* Kill an order that never reached the book, returning the escrow submit reserved for it.
	*  Uses the same refund arithmetic as OrderBook::cancelOrder so the two paths agree. */
	void killUnplaced(const std::shared_ptr<Order>& order, const std::shared_ptr<Agent>& agent);
};
