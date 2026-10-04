#pragma once
#include <unordered_map>
#include <deque>
#include <memory>
#include <cmath>
#include <numeric>
#include <chrono>
#include <set>
#include "Holding.h"
#include "Enums.h" // OrderAction, for TradePrint::aggressor
#include "Order.h" // for order queues
#include "Snapshot.h"
#include "Ledger.h"
#include "Features.h"
#include "StockLoan.h"

class Agent;
class SimClock;
enum class OrderAction;
enum class OrderType;
enum class ID_TYPE;
enum class Session;

/* One executed trade, recorded at the moment the leg executes
*
* A sweep that walks several price levels produces one of these PER LEVEL, each carrying
* the price that level actually traded at -- not a single post-sweep last price. This is
* the stream a chart, a volume histogram and any VWAP are all built from, so it records
* what happened rather than what the book looked like afterwards.
*
* `aggressor` is the side that crossed the spread to make the trade happen: BID when a
* buyer lifted an offer, ASK when a seller hit a bid. The resting side is the opposite.
*/
/* Default ceiling on retained trade prints
*
* Sized so an ordinary multi-day run backfills a chart without ever truncating: a 3 day
* run at 1,000 residents produces roughly 22,000 trades, so this holds about a month of
* one. At 24 bytes a print that is ~6 MB, which is the point -- the history used to be
* an unbounded vector and a long run grew it forever.
*/
inline constexpr std::size_t TICK_HISTORY_MAX = 250'000;

struct TradePrint {
	double price;
	unsigned int volume;
	double timeMs;
	OrderAction aggressor;
};
struct CompareBid {
public:
	bool operator()(const std::shared_ptr<Order>& o1, const std::shared_ptr<Order>& o2) const {
		if (o1->price != o2->price) {
			return o1->price > o2->price; // higher price first
		}
		if (o1->timestamp != o2->timestamp) {
			return o1->timestamp < o2->timestamp; // earlier timestamp first (FIFO)
		}
		return o1->id < o2->id; // unique tiebreak, ids are zero padded so this is creation order
	}
};
struct CompareAsk {
public:
	bool operator()(const std::shared_ptr<Order>& o1, const std::shared_ptr<Order>& o2) const {
		if (o1->price != o2->price) {
			return o1->price < o2->price; // lower price first
		}
		if (o1->timestamp != o2->timestamp) {
			return o1->timestamp < o2->timestamp; // earlier timestamp first (FIFO)
		}
		return o1->id < o2->id; // unique tiebreak, ids are zero padded so this is creation order
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
	unsigned int shareFloat;
	/* Simulation Clock */
	SimClock* clock;
	/* Executed trades since the start
	*
	* One per trade, NOT one per order completion -- an order that sweeps four price
	* levels counts four. The names `tickCount` and `tickHistory` predate that meaning
	* and are kept so the terminal UI needs no edit, but a "tick" here is a trade.
	*/
	long long tickCount;
	/* The overall neutral sentiment value for all agents in the market */
	double marketNeutralSentiment;
	/* Retained trade prints, oldest first
	*
	* A deque, and trimmed from the front once it passes tickHistoryMax, so memory is
	* bounded on an arbitrarily long run. It is therefore a WINDOW on the run's trades,
	* not all of them -- tickCount remains the true total, and the two diverge once the
	* cap is reached. Anything reporting how much has traded wants tickCount; anything
	* drawing recent history wants this.
	*/
	std::deque<TradePrint> tickHistory;
	/* Ceiling on retained prints, 0 disables trimming and restores unbounded growth */
	std::size_t tickHistoryMax = TICK_HISTORY_MAX;
	/* Priority set for bid limit orders */
	std::set<std::shared_ptr<Order>, CompareBid> bidQueue;
	/* Priority set for ask limit orders */
	std::set<std::shared_ptr<Order>, CompareAsk> askQueue;
	/* Log of all orders placed by agents || OrderId: Order */
	//std::unordered_map<std::string, std::shared_ptr<Order>> orderHistory;
	/* Log of all agents in the sim || AgentId: Agent */
	std::unordered_map<std::string, std::shared_ptr<Agent>> agents;
	/* Agents whose resting order was just hit and that should be re-scheduled immediately
	*
	* Backed off quoting agents would otherwise wait out their idle timer before
	* replacing a filled quote. Drained by CoreSim after each event.
	*/
	std::vector<std::string> wakeQueue;
	/* Every dollar that enters, leaves, or moves outside a trade. See Ledger.h. */
	Ledger ledger;
	/* Which order model mechanisms are switched on. Configuration, so a reset keeps it. */
	Features features;
	/* The pool short sellers borrow from (OrderModelPlan Step 1.3) */
	StockLoan lending;
	/* Unit-dollars-to-money multiplier for this run, mirrored from CoreSim::cashScale so the
	*  account rules can test a dollar threshold against the person an agent stands for */
	double cashScale = 1.0;

	// ---- Trigger pump inputs (OrderModelPlan Step 1.2) ----

	/* Lowest and highest price printed since the broker last looked. recordTrade widens it and
	*  never acts on it: triggers fire only after matching has returned. */
	bool printRangeValid = false;
	double printLow = 0.0;
	double printHigh = 0.0;
	/* Accounts whose cash or position changed since the broker last looked, in change order */
	std::vector<std::string> dirtyAccounts;
	/* How deep inside MatchingEngine::match the engine currently is. The broker refuses to
	*  fire a trigger while this is non-zero -- that is the whole re-entrancy rule. */
	int matchDepth = 0;

	OrderBook() = default;
	OrderBook(double currentPrice, unsigned int shareFloat = 0);

// ---- Agent Operations ----
	/* Update or insert an agent to the agents map */
	void upsertAgent(std::shared_ptr<Agent> agent);
	/* Look up an agent by id, returns nullptr when there is no such agent
	*
	* Always use this rather than agents[id]. operator[] default constructs a null
	* shared_ptr for a missing key, so a lookup that misses silently INSERTS a null
	* entry: the map grows, agents.size() is wrong, and the next sweep dereferences it.
	*/
	std::shared_ptr<Agent> getAgent(const std::string& agentId) const;
	/* Whether this agent currently has any order resting in either queue
	*
	* An Order carries only its agent's id, so an id that is reused while the previous
	* holder still has orders in the book would hand those fills and their escrow to
	* the wrong agent. This is the precondition an agent id must satisfy before it can
	* be recycled.
	*/
	bool agentHasRestingOrders(const std::string& agentId) const;

// ---- Order Operations ----
	/* Get the best active bid/ask and remove it from the queue */
	std::shared_ptr<Order> getBest(OrderAction side);
	/* Get the best n active bid/ask without removing it from the queue */
	std::vector<std::shared_ptr<Order>> peekBestN(OrderAction side, unsigned char n = 1);
	/* Add a new order to the bid/ask queue and orderbook */
	void addOrder(std::shared_ptr<Order> order);
	/* Remove an order from the bid/ask queue, update the status to canceled, return assets if applicable */
	void cancelOrder(std::shared_ptr<Order> order, std::shared_ptr<Agent> agent);
	/* Order was filled, remove from queue, update status to CLOSED */
	void fillOrder(std::shared_ptr<Order> order, int volFilled);
	/* Take a resting order out of its queue without cancelling it or touching its escrow
	*
	* For a replace that loses priority: the order has to leave the set while its price and
	* timestamp are still the ones it was sorted by, or the erase misses. Returns whether it
	* was there. The caller re-enters it, through matching, once its new key is set.
	*/
	bool removeFromQueue(const std::shared_ptr<Order>& order);
	/* Record one executed trade leg and count it
	*
	* Called by the matching engine once per leg, from inside the matching loop where the
	* execution price, the volume and the crossing side are all still known. The price is
	* passed explicitly rather than read from currentPrice so the record cannot drift with
	* the order in which a caller updates the last price.
	*
	* This is the ONLY place tickCount moves. It used to be incremented from fillOrder
	* (which sees a resting order closing, and cannot know who crossed) plus a
	* compensating push after each matching loop -- an arrangement that lost a print
	* whenever a cash-constrained aggressor left its last leg partially filled.
	*/
	void recordTrade(double price, unsigned int volume, OrderAction aggressor);
	/* Cancel every resting order whose expiry has been reached, return how many were expired
	*
	* Called when an expiring session boundary is crossed. Every cancellation goes
	* through cancelOrder so escrowed cash and reserved shares are returned.
	*/
	unsigned int expireOrders(double nowMs);

// ---- Utility Operations ----
	/* Update current price and tick precision */
	void updateCurrentPrice(double newPrice);
	/* Make a unique id for an Agent or Order */
	std::string makeId(ID_TYPE type);
	/* Get a snapshot of the current state of the Order Book */
	Snapshot getSnapshot(unsigned char depth = 10);
	/* Prints RETAINED since startTick, not trades since then
	*
	* Counts entries still held in tickHistory, so once the cap is reached this is a
	* window length rather than a total. Use tickCount for the run's real trade count.
	*/
	int getTick(int startTick = 0);
	/* Get the number of open bids resting in the book */
	int getNumBids() const { return this->numBids; }
	/* Get the number of open asks resting in the book */
	int getNumAsks() const { return this->numAsks; }
	/* Resets the LOB to its initial state */
	void resetToInitial(double initialPrice, unsigned int shareFloat = 0, bool clearAgents = false);

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

// ---- Utility Operations ----
	/* Creates a random ticker symbol */
	std::string makeTickerSymbol();
	/* Set the decimal precision to use based on current price */
	void setTickPrecision(double price);
	/* Helper function for removing best order from bid/ask queue */
	template <typename T>
	std::shared_ptr<Order> removeBestFrom(T& queue) {
		while (!queue.empty()) {
			std::shared_ptr<Order> best = *queue.begin();

			queue.erase(queue.begin());

			if (best->status != OrderStatus::CANCELED) {
				return best;
			}
		}
		return nullptr;
	}
	/* Helper function for peeking the best order(s) from bid/ask queue */
	template <typename T>
	std::vector<std::shared_ptr<Order>> peekBestFrom(T& queue, unsigned char n) {
		std::vector<std::shared_ptr<Order>> bestN;

		auto it = queue.begin();
		while (it != queue.end() && bestN.size() < n) {
			std::shared_ptr<Order> order = *it;

			if (order->status != OrderStatus::CANCELED) {
				bestN.push_back(order);
			}

			++it;
		}

		return bestN;
	}
};

