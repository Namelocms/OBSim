#pragma once
#include <string>
#include <vector>

#include "Enums.h"
#include "OrderBook.h"   // TradePrint
#include "CoreSim.h"     // BackDataProgress

/* ---- The frontend's view of the simulation ----
*
* One MarketFrame is everything a frontend needs to draw one update. It is a plain value:
* no pointers into the engine, no shared_ptr to anything the sim thread will mutate, no
* locks held by whoever received it. That is the whole point -- a frame can be copied,
* queued, serialised and sent without the engine caring, which is what makes a frontend
* over a socket possible at all.
*
* Frames are built on the SIM THREAD, between engine operations, because the engine has
* no internal locking and there is no other moment at which its state is coherent.
*/

/* One aggregated price level of the book, not one order
*
* The terminal UI renders the top N *orders*, which double-prints a level that several
* agents are quoting at. A depth ladder wants the level. `orders` is kept so the
* difference stays visible rather than being silently averaged away.
*/
struct BookLevel {
	double price = 0.0;
	unsigned long long volume = 0;
	int orders = 0;
};

/* One agent, as far as a frontend is concerned */
struct FrameAgentRow {
	std::string id;
	double cash = 0.0;
	unsigned int holdings = 0;
	int numBids = 0;
	int numAsks = 0;
	double sentiment = 0.0;
	AgentStatus status = AgentStatus::INACTIVE;
	AgentType type = AgentType::RETAIL;
	AgentSubType subType = AgentSubType::NOISE;
	bool isTransient = false;
	/* Derived, not a status of its own: a LEAVING agent past its unwind window */
	bool isStranded = false;

	// ---- The account (OrderModelPlan Step 4.1) ----
	/* Worth at the last price: cash, escrow and long shares, less shares owed */
	double equity = 0.0;
	/* Shares sold short and still owed, and how many of those are on loan */
	unsigned int shortShares = 0;
	unsigned int borrowedShares = 0;
	double buyingPower = 0.0;
	/* Has the equity to borrow under the margin rules */
	bool marginPrivileges = false;
	/* Below its maintenance requirement right now */
	bool inViolation = false;
	/* Stops and other orders the broker is holding for it */
	int heldOrders = 0;
};

/* The state of the market's structure, rather than of its price (OrderModelPlan Step 4.1) */
struct FrameMarketState {
	// LULD
	bool luldActive = false;
	double luldLower = 0.0;
	double luldUpper = 0.0;
	double luldReference = 0.0;
	bool limitState = false;
	bool paused = false;
	double pauseEndsMs = 0.0;
	// Short sale restriction
	bool ssrActive = false;
	double ssrUntilMs = 0.0;
	double ssrReferenceClose = 0.0;
	// Official prices
	double officialOpen = 0.0;
	double officialClose = 0.0;
	double previousClose = 0.0;
	// The next cross, while it is collecting: before the open, and after the on-close cutoff
	bool auctionCollecting = false;
	bool auctionIsOpen = false;
	double indicativePrice = 0.0;
	unsigned long long indicativeMatched = 0;
	unsigned long long imbalance = 0;
	OrderAction imbalanceSide = OrderAction::BID;
	int auctionOrders = 0;
};

/* The lending pool */
struct FrameLending {
	double supply = 0.0;
	unsigned long long borrowed = 0;
	double utilisation = 0.0;
	double feeRate = 0.0;
	/* Shares sold short across every account, borrowed or not */
	unsigned long long shortInterest = 0;
};

/* What the house has taken: the ledger's accounts */
struct FrameHouse {
	double commissions = 0.0;
	double exchangeFees = 0.0;
	double regulatoryFees = 0.0;
	double marginInterest = 0.0;
	double borrowFees = 0.0;
	double brokerLosses = 0.0;
};

/* Running counts of what the broker has done over the run */
struct FrameBrokerCounts {
	long long marginCalls = 0;
	long long writeOffs = 0;
	long long stopsTriggered = 0;
	long long cascades = 0;
	int deepestCascade = 0;
	long long recalledShares = 0;
	long long buyIns = 0;
	long long brackets = 0;
	long long tradingPauses = 0;
	long long ssrTriggers = 0;
};

/* One line of the event log, with its time kept separate from its text */
struct FrameLogLine {
	double simTimeMs = 0.0;
	LogEntry::Kind kind = LogEntry::Kind::HOLD;
	std::string text;
};

/* A bulk copy of the retained trade history, for a client that joined mid-run
*
* Separate from MarketFrame because it is sent once on connect rather than every frame,
* and because it is large: up to the whole retention window.
*/
struct TradeBackfill {
	std::vector<TradePrint> trades;
	/* Total trades executed over the run at the moment of capture. Compare against
	*  trades.size() to see how much of the run the window still reaches back over. */
	long long tickCountAtCapture = 0;
	/* True when the history no longer reaches the start of the run, either because the
	*  retention cap trimmed it or because the caller asked for fewer than it holds. A
	*  chart should say "history begins here" rather than implying the run did. */
	bool truncated = false;
};

/* One of the user's working orders (OrderModelPlan Step 4.2) */
struct FrameUserOrder {
	std::string id;
	OrderAction side = OrderAction::BID;
	OrderType type = OrderType::LIMIT;
	double price = 0.0;
	double stopPrice = 0.0;
	double trailAmount = 0.0;
	double trailPercent = 0.0;
	/* Still to fill, and as entered */
	unsigned int volume = 0;
	unsigned int entryVolume = 0;
	TimeInForce tif = TimeInForce::DAY;
	/* Resting in the book, held by the broker until it triggers, or waiting for a cross */
	bool held = false;
	bool inAuction = false;
	bool shortSale = false;
	bool hidden = false;
	unsigned int displayQty = 0;
	bool midpointPeg = false;
	bool extendedHours = false;
	/* The OCO or bracket it belongs to */
	std::string groupId;
};

/* One of the user's executions */
struct FrameUserFill {
	double timeMs = 0.0;
	std::string orderId;
	OrderAction side = OrderAction::BID;
	double price = 0.0;
	unsigned int volume = 0;
	double fee = 0.0;
	bool maker = false;
	bool auction = false;
	bool shortSale = false;
};

/* The user's account, when the run has one */
struct FrameUser {
	bool present = false;
	/* Real-world dollars to sim dollars for this account: 1 when it is in dollars as entered */
	double moneyScale = 1.0;
	bool scaled = true;
	std::string broker;
	double cash = 0.0;
	double escrow = 0.0;
	double equity = 0.0;
	double buyingPower = 0.0;
	/* Owed to the broker on margin */
	double debit = 0.0;
	unsigned long long longShares = 0;
	unsigned int shortShares = 0;
	unsigned int borrowedShares = 0;
	bool marginPrivileges = false;
	bool inViolation = false;
	/* The open position at its average cost, and P&L on it (fees separately) */
	long long position = 0;
	double averageCost = 0.0;
	double realizedPnl = 0.0;
	double unrealizedPnl = 0.0;
	double feesPaid = 0.0;
	std::vector<FrameUserOrder> orders;
	/* Executions since the last frame */
	std::vector<FrameUserFill> fills;
	long long fillsDropped = 0;
};

struct MarketFrame {
	// ---- Sequencing ----
	/* Monotonic frame number, so a consumer can detect a gap it was never sent */
	unsigned long long sequence = 0;

	/* Sim time this frame describes, in milliseconds since t = 0
	*
	* AUTHORITATIVE. epochSec below is derived from it and is lossy -- see
	* MarketCalendar::simTimeToEpochSec. Anything reasoning about when something happened
	* uses this; epochSec exists so a chart can put a date on an axis.
	*/
	double simTimeMs = 0.0;
	/* simTimeMs mapped onto a Unix timestamp, for DRAWING only */
	double epochSec = 0.0;

	// ---- Headline ----
	double currentPrice = 0.0;
	double spread = 0.0;
	Session session = Session::PREMARKET;
	double marketNeutralSentiment = 0.0;
	unsigned int shareFloat = 0;
	/* Executed trades over the whole run. Not tickHistory.size(), which is a window. */
	long long tickCount = 0;
	int restingBids = 0;
	int restingAsks = 0;

	// ---- Population ----
	int residentAgents = 0;
	int liveTransients = 0;
	int totalAgents = 0;
	/* This run's own drawn transient fraction, 0 when the feature is off */
	double transientFraction = 0.0;

	// ---- Clock and run state ----
	double speedMultiplier = 1.0;
	bool paused = false;
	bool running = false;
	bool backDataRunning = false;
	bool backDataAborted = false;
	/* Only meaningful while backDataRunning */
	BackDataProgress backData;

	// ---- Market structure, lending, the house, the broker (OrderModelPlan Step 4.1) ----
	FrameMarketState market;
	FrameLending lending;
	FrameHouse house;
	FrameBrokerCounts broker;
	/* The user's account (OrderModelPlan Step 4.2) */
	FrameUser user;

	// ---- Book depth, best first ----
	std::vector<BookLevel> bids;
	std::vector<BookLevel> asks;

	// ---- Agents ----
	/* Agent rows are published on a slower cadence than the frame rate: they change slowly
	*  and sorting a large population every frame is the most expensive thing here. False
	*  means "unchanged, reuse what you have", NOT "there are no agents". */
	bool agentsIncluded = false;
	/* Rows actually carried, residents first then transients, each by id */
	std::vector<FrameAgentRow> agents;
	/* Agents that exist but did not fit the row cap */
	int agentsOmitted = 0;

	// ---- Batched since the previous frame ----
	/* Trades executed since the last frame, oldest first
	*
	* The client builds candles and indicators from these, so they are the payload that
	* actually matters. A frame carrying none is normal -- most frames do.
	*/
	std::vector<TradePrint> trades;
	/* Trades that happened but could not be carried, because more executed between frames
	*  than the retained history holds. Non-zero means the client's candles have a hole in
	*  them and it should say so rather than drawing through it. */
	long long tradesDropped = 0;

	/* Log lines since the last frame, oldest first */
	std::vector<FrameLogLine> logs;
	/* Lines dropped to the per-frame cap. onLog fires on EVERY agent event, so at speed
	*  this is routinely non-zero and is not an error -- it is the cap doing its job. */
	long long logsDropped = 0;
};
