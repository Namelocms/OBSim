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
};

/* One line of the event log, with its time kept separate from its text */
struct FrameLogLine {
	double simTimeMs = 0.0;
	LogEntry::Kind kind = LogEntry::Kind::HOLD;
	std::string text;
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
