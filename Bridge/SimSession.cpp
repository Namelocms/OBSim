#include "include/SimSession.h"

#include <algorithm>
#include <queue>

#include "Agent.h"
#include "Order.h"
#include "OrderBook.h"
#include "Enums.h"
#include "Account.h"
#include "Broker.h"
#include "MatchingEngine.h"

// ============================================================
//  Construction
// ============================================================

SimSession::SimSession(CoreSim& sim, SimClock& clock)
	: sim_(sim), clock_(clock) {
	this->lastPublish_ = std::chrono::steady_clock::now();
	this->lastAgentPublish_ = this->lastPublish_;
}

SimSession::~SimSession() {
	this->stop();
}

// ============================================================
//  Lifecycle
// ============================================================

void SimSession::start(const SimParams& params) {
	this->stop();

	this->sim_.setParameters(
		params.seed, params.backDataDays, params.liveStartSession, params.minLiquidity,
		params.agentCount, params.shareFloat, params.startPrice, params.transientFraction);
	this->sim_.setFeatures(params.features);

	// Reset everything derived from the previous run, or the first frame of the new one
	// reports the old one's trades and log lines.
	{
		std::lock_guard<std::mutex> lk(this->frameMtx_);
		this->latestFrame_ = MarketFrame{};
		this->hasFrame_ = false;
		this->sequence_ = 0;
		this->backData_ = BackDataProgress{};
	}
	{
		std::lock_guard<std::mutex> lk(this->logMtx_);
		this->pendingLogs_.clear();
		this->pendingLogsDropped_ = 0;
	}
	this->lastTradeCount_ = 0;
	this->everPublished_ = false;
	this->lastPublish_ = std::chrono::steady_clock::now();
	this->lastAgentPublish_ = this->lastPublish_;

	this->sim_.onTick = [this]() { this->onTick_(); };
	this->sim_.onIdle = [this]() { this->onIdle_(); };
	this->sim_.onLog = [this](LogEntry e) { this->onLog_(e); };
	this->sim_.onBackDataProgress = [this](BackDataProgress p) { this->onBackDataProgress_(p); };

	this->finished_.store(false);
	this->threadRunning_.store(true);
	this->thread_ = std::thread([this]() {
		this->sim_.run(this->clock_);
		// One last frame so a consumer sees the final state rather than the second to last
		this->publish_();
		this->threadRunning_.store(false);
		this->finished_.store(true);
		});
}

void SimSession::stop() {
	if (!this->thread_.joinable()) { return; }

	// Order matters. Raise the flags first so the loop breaks out on its own terms, then
	// unblock anything parked: a paused clock never returns from its own wait, and the
	// back-data run only checks cancelRequested on an interval.
	this->sim_.isRunning = false;
	this->sim_.cancelRequested.store(true);
	this->clock_.resume();

	this->thread_.join();
	this->threadRunning_.store(false);

	// Only now is it safe to touch the queue -- we are the last thread standing. Swap
	// rather than pop, which is O(1) against a queue that may hold every agent.
	std::priority_queue<EventCall, std::vector<EventCall>, CompareEventCalls> empty;
	std::swap(this->sim_.eventCallQueue, empty);

	// Drop the callbacks so a later run cannot deliver into a half torn down session
	this->sim_.onTick = nullptr;
	this->sim_.onIdle = nullptr;
	this->sim_.onLog = nullptr;
	this->sim_.onBackDataProgress = nullptr;

	this->sim_.cancelRequested.store(false);
}

void SimSession::restart(const SimParams& params) {
	this->stop();
	this->start(params);
}

// ============================================================
//  Reading
// ============================================================

bool SimSession::latest(MarketFrame& out) const {
	std::lock_guard<std::mutex> lk(this->frameMtx_);
	if (!this->hasFrame_) { return false; }
	out = this->latestFrame_;
	return true;
}

unsigned long long SimSession::latestSequence() const {
	std::lock_guard<std::mutex> lk(this->frameMtx_);
	return this->hasFrame_ ? this->sequence_ : 0ull;
}

void SimSession::requestBackfill(size_t maxTrades) {
	if (maxTrades == 0) { return; }
	// Widen an outstanding request rather than replacing it, so two clients asking for
	// different depths at once both get what they need from one capture.
	size_t current = this->backfillWanted_.load();
	while (maxTrades > current
		&& !this->backfillWanted_.compare_exchange_weak(current, maxTrades)) {
		// current is refreshed by the failed exchange
	}
}

bool SimSession::takeBackfill(TradeBackfill& out) {
	std::lock_guard<std::mutex> lk(this->backfillMtx_);
	if (!this->backfillReady_) { return false; }
	out = std::move(this->backfillData_);
	this->backfillData_ = TradeBackfill{};
	this->backfillReady_ = false;
	return true;
}

void SimSession::fulfilBackfill_() {
	// A request made while back data runs -- every reset makes one, and so does a client
	// connecting mid-run -- waits for the handoff. Captured now it would hold the first few
	// trades of a history the chart is about to need in full, and nothing asks again.
	if (this->sim_.backDataRunning.load()) { return; }
	const size_t wanted = this->backfillWanted_.exchange(0);
	if (wanted == 0) { return; }

	const OrderBook& ob = this->sim_.OB;
	const size_t retained = ob.tickHistory.size();
	const size_t take = (wanted < retained) ? wanted : retained;

	TradeBackfill data;
	data.tickCountAtCapture = ob.tickCount;
	data.truncated = ((long long)take < ob.tickCount);
	data.trades.reserve(take);
	for (size_t i = retained - take; i < retained; ++i) {
		data.trades.push_back(ob.tickHistory[i]);
	}

	std::lock_guard<std::mutex> lk(this->backfillMtx_);
	this->backfillData_ = std::move(data);
	this->backfillReady_ = true;
}

// ============================================================
//  Controls
// ============================================================

void SimSession::pause() { this->clock_.pause(); }
void SimSession::resume() { this->clock_.resume(); }

void SimSession::togglePause() {
	if (this->clock_.paused.load()) { this->clock_.resume(); }
	else { this->clock_.pause(); }
}

void SimSession::step() {
	if (!this->clock_.paused.load()) { this->clock_.pause(); }
	this->clock_.step.store(true);
}

void SimSession::setSpeed(double multiplier) {
	if (!(multiplier > 0.0)) { return; }
	// Through setSpeed, not a bare store: it stores the new multiplier BEFORE rebasing,
	// and rebasing against the old one leaves the target scaled wrong and produces the
	// catch-up burst it exists to prevent.
	this->clock_.setSpeed(multiplier);
}

void SimSession::setSentiment(double value) {
	this->sim_.OB.marketNeutralSentiment = std::clamp(value, -1.0, 1.0);
}

void SimSession::nudgeSentiment(double delta) {
	this->setSentiment(this->sim_.OB.marketNeutralSentiment + delta);
}

void SimSession::cancelBackData() {
	this->sim_.cancelRequested.store(true);
}

// ============================================================
//  Engine callbacks -- all on the sim thread
// ============================================================

void SimSession::onTick_() { this->maybePublish_(); }
void SimSession::onIdle_() { this->maybePublish_(); }

void SimSession::onLog_(LogEntry entry) {
	std::lock_guard<std::mutex> lk(this->logMtx_);
	if ((int)this->pendingLogs_.size() >= this->config.logLinesPerFrame) {
		++this->pendingLogsDropped_;
		return;
	}
	FrameLogLine line;
	line.simTimeMs = entry.simTimeMs;
	line.kind = entry.kind;
	line.text = std::move(entry.text);
	this->pendingLogs_.push_back(std::move(line));
}

void SimSession::onBackDataProgress_(BackDataProgress progress) {
	{
		std::lock_guard<std::mutex> lk(this->frameMtx_);
		this->backData_ = progress;
	}
	// Back data is headless and fires no ticks, so without publishing from here a
	// consumer would see nothing at all until the live loop started.
	this->maybePublish_();
}

// ============================================================
//  Publishing
// ============================================================

void SimSession::maybePublish_() {
	if (!(this->config.framesPerSecond > 0.0)) { return; }

	const auto now = std::chrono::steady_clock::now();
	const double intervalMs = 1000.0 / this->config.framesPerSecond;
	const double sinceMs = std::chrono::duration<double, std::milli>(now - this->lastPublish_).count();

	// Always publish the first frame, so a consumer that connects to a quiet market is not
	// left with nothing until something happens to trade.
	if (this->everPublished_ && sinceMs < intervalMs) { return; }

	this->publish_();
}

void SimSession::publish_() {
	const auto now = std::chrono::steady_clock::now();

	MarketFrame frame;
	frame.simTimeMs = this->clock_.simTimeMs;
	frame.epochSec = MarketCalendar::simTimeToEpochSec(frame.simTimeMs);

	const OrderBook& ob = this->sim_.OB;
	frame.currentPrice = ob.currentPrice;
	frame.session = ob.session;
	frame.marketNeutralSentiment = ob.marketNeutralSentiment;
	frame.shareFloat = ob.shareFloat;
	frame.tickCount = ob.tickCount;
	frame.restingBids = ob.getNumBids();
	frame.restingAsks = ob.getNumAsks();

	frame.residentAgents = this->sim_.residentCount;
	frame.liveTransients = this->sim_.liveTransientCount;
	frame.totalAgents = frame.residentAgents + frame.liveTransients;
	frame.transientFraction = this->sim_.runTransientFraction;

	frame.speedMultiplier = this->clock_.speedMultiplier.load();
	frame.paused = this->clock_.paused.load();
	frame.running = this->sim_.isRunning.load();
	frame.backDataRunning = this->sim_.backDataRunning.load();
	frame.backDataAborted = this->sim_.backDataAborted;

	this->fillBook_(frame);
	this->fillTrades_(frame);
	this->fillMarket_(frame);
	this->fulfilBackfill_();

	// Spread comes from the aggregated book rather than Snapshot, so it agrees with the
	// ladder the frontend is drawing instead of being computed off a different read.
	if (!frame.bids.empty() && !frame.asks.empty()) {
		frame.spread = frame.asks.front().price - frame.bids.front().price;
	}

	// Roster on its own slower cadence
	const double agentIntervalMs = (this->config.agentRowsPerSecond > 0.0)
		? 1000.0 / this->config.agentRowsPerSecond : 0.0;
	const double sinceAgentsMs =
		std::chrono::duration<double, std::milli>(now - this->lastAgentPublish_).count();
	if (agentIntervalMs > 0.0 && (!this->everPublished_ || sinceAgentsMs >= agentIntervalMs)) {
		this->fillAgents_(frame);
		this->lastAgentPublish_ = now;
	}

	{
		std::lock_guard<std::mutex> lk(this->logMtx_);
		frame.logs = std::move(this->pendingLogs_);
		frame.logsDropped = this->pendingLogsDropped_;
		this->pendingLogs_.clear();
		this->pendingLogsDropped_ = 0;
	}

	{
		std::lock_guard<std::mutex> lk(this->frameMtx_);
		frame.backData = this->backData_;
		frame.sequence = ++this->sequence_;
		this->latestFrame_ = std::move(frame);
		this->hasFrame_ = true;
	}

	this->lastPublish_ = now;
	this->everPublished_ = true;
}

// ============================================================
//  Frame contents
// ============================================================

void SimSession::fillBook_(MarketFrame& frame) const {
	const int maxLevels = (this->config.bookDepthLevels > 0) ? this->config.bookDepthLevels : 0;
	if (maxLevels == 0) { return; }

	// Both queues are ordered best first, so equal prices are adjacent and a level is
	// finished the moment the price changes. Walking them directly rather than through
	// peekBestN because that returns the best N ORDERS -- several agents quoting one price
	// would eat the depth and the ladder would show a handful of levels.
	auto collect = [maxLevels](auto& queue, std::vector<BookLevel>& out) {
		out.reserve(maxLevels);
		for (const std::shared_ptr<Order>& order : queue) {
			if (order == nullptr || order->status == OrderStatus::CANCELED) { continue; }
			// The ladder is the displayed book: hidden orders never show, reserve orders only
			// show their tip (OrderModelPlan Step 3.4)
			if (!order->isDisplayed()) { continue; }
			if (!out.empty() && out.back().price == order->price) {
				out.back().volume += order->displayedVolume();
				out.back().orders += 1;
				continue;
			}
			if ((int)out.size() >= maxLevels) { break; }
			BookLevel level;
			level.price = order->price;
			level.volume = order->displayedVolume();
			level.orders = 1;
			out.push_back(level);
		}
		};

	collect(this->sim_.OB.bidQueue, frame.bids);
	collect(this->sim_.OB.askQueue, frame.asks);
}

void SimSession::fillTrades_(MarketFrame& frame) {
	const OrderBook& ob = this->sim_.OB;

	long long since = ob.tickCount - this->lastTradeCount_;
	if (since <= 0) {
		this->lastTradeCount_ = ob.tickCount;   // a reset can move it backwards
		return;
	}

	// The retained history is a window. If more trades executed between frames than it
	// holds, the older ones are simply gone -- say so rather than silently shipping a
	// shorter list and letting the client draw straight through the hole.
	const long long retained = (long long)ob.tickHistory.size();
	if (since > retained) {
		frame.tradesDropped = since - retained;
		since = retained;
	}

	frame.trades.reserve((size_t)since);
	const size_t start = (size_t)(retained - since);
	for (size_t i = start; i < ob.tickHistory.size(); ++i) {
		frame.trades.push_back(ob.tickHistory[i]);
	}

	this->lastTradeCount_ = ob.tickCount;
}

void SimSession::fillAgents_(MarketFrame& frame) const {
	frame.agentsIncluded = true;

	// POOLED slots are skipped: they are empty seats waiting to be rerolled, not
	// participants, and counting them would make the roster climb forever.
	std::vector<std::shared_ptr<Agent>> residents, transients;
	residents.reserve(this->sim_.OB.agents.size());
	for (const auto& entry : this->sim_.OB.agents) {
		const std::shared_ptr<Agent>& agent = entry.second;
		if (agent == nullptr || agent->status == AgentStatus::POOLED) { continue; }
		(agent->isTransient ? transients : residents).push_back(agent);
	}

	// Sorted, residents first. Two reasons, one visible and one not. The visible one: the
	// default page does not churn as transients arrive and leave, and transient rows sit
	// together at the end where they are worth watching. The other: unordered_map order is
	// implementation defined and a rehash may change it, so sorting makes the roster
	// deterministic rather than relying on this build happening to iterate in insertion
	// order.
	auto byId = [](const std::shared_ptr<Agent>& a, const std::shared_ptr<Agent>& b) {
		return a->id < b->id;
		};
	std::sort(residents.begin(), residents.end(), byId);
	std::sort(transients.begin(), transients.end(), byId);

	const int cap = (this->config.agentRowCap > 0) ? this->config.agentRowCap : 0;
	const int total = (int)(residents.size() + transients.size());
	if (cap == 0) {
		frame.agentsOmitted = total;
		return;
	}

	// Transients are few and are the rows that churn, so they get their places first and
	// residents fill what is left. Truncating in display order instead would cut the
	// transients off entirely the moment the resident population exceeds the cap -- which
	// is exactly when someone is most likely to be watching for them.
	const int transientTake = std::min((int)transients.size(), cap);
	const int residentTake = std::max(0, cap - transientTake);

	frame.agents.reserve((size_t)std::min(cap, total));
	const double nowMs = this->clock_.simTimeMs;

	auto append = [&](const std::vector<std::shared_ptr<Agent>>& from, int take) {
		for (const std::shared_ptr<Agent>& agent : from) {
			if (take-- <= 0) { return; }
			FrameAgentRow row;
			row.id = agent->id;
			row.cash = agent->cash;
			row.holdings = agent->getTotalHoldings();
			row.numBids = (int)agent->activeBids.size();
			row.numAsks = (int)agent->activeAsks.size();
			row.sentiment = agent->sentiment;
			row.status = agent->status;
			row.type = agent->type;
			row.subType = agent->subType;
			row.isTransient = agent->isTransient;
			row.isStranded = (agent->status == AgentStatus::LEAVING) && agent->isStranded(nowMs);
			const double price = this->sim_.OB.currentPrice;
			row.equity = Account::equity(*agent, price);
			row.shortShares = agent->shortShares;
			row.borrowedShares = agent->borrowedShares;
			row.buyingPower = Account::buyingPower(*agent);
			row.marginPrivileges = Account::hasMarginPrivileges(*agent);
			row.inViolation = Account::inMaintenanceViolation(*agent, price);
			row.heldOrders = (int)agent->heldOrders.size();
			frame.agents.push_back(std::move(row));
		}
		};
	// Appended residents first so display order still reads residents then transients,
	// even though transients were the ones guaranteed a place.
	append(residents, residentTake);
	append(transients, transientTake);

	frame.agentsOmitted = total - (int)frame.agents.size();
}

void SimSession::fillMarket_(MarketFrame& frame) const {
	const OrderBook& ob = this->sim_.OB;
	const double now = this->clock_.simTimeMs;

	FrameMarketState& m = frame.market;
	m.luldActive = ob.luld.active;
	m.luldLower = ob.luld.lower;
	m.luldUpper = ob.luld.upper;
	m.luldReference = ob.luld.reference;
	m.limitState = ob.luld.limitStateSince >= 0.0;
	m.paused = ob.luld.paused;
	m.pauseEndsMs = ob.luld.pauseEndsMs;
	m.ssrActive = ob.ssrActive(now);
	m.ssrUntilMs = ob.ssrUntilMs;
	m.ssrReferenceClose = ob.ssrReferenceClose;
	m.officialOpen = ob.officialOpen;
	m.officialClose = ob.officialClose;
	m.previousClose = ob.previousClose;

	// The next cross is worth showing while it is collecting: through the premarket for the
	// open, and from the on-close cutoff for the close -- when the real venues publish theirs
	if (ob.features.auctions.enabled) {
		const int day = MarketCalendar::dayIndex(now);
		const double close = MarketCalendar::sessionOpenMs(Session::REGULAR, day) + MarketCalendar::sessionLengthMs(Session::REGULAR);
		const bool beforeOpen = (ob.session == Session::PREMARKET);
		const bool beforeClose = (ob.session == Session::REGULAR)
			&& now >= close - MarketCalendar::minutesToMs(CLOSE_ORDER_CUTOFF_MINUTES);
		if (beforeOpen || beforeClose) {
			const TimeInForce which = beforeOpen ? TimeInForce::OPG : TimeInForce::CLS;
			CrossResult cross = this->sim_.ME.indicativeCross(which, ob.currentPrice);
			m.auctionCollecting = true;
			m.auctionIsOpen = beforeOpen;
			m.indicativePrice = cross.price;
			m.indicativeMatched = cross.matched;
			m.imbalance = cross.imbalance;
			m.imbalanceSide = cross.imbalanceSide;
			int queued = 0;
			for (const auto& o : ob.auctionOrders) { if (o->tif == which) { ++queued; } }
			m.auctionOrders = queued;
		}
	}

	FrameLending& l = frame.lending;
	l.supply = ob.lending.supply();
	l.borrowed = ob.lending.borrowed;
	l.utilisation = ob.lending.utilisation();
	l.feeRate = StockLoan::feeRate(l.utilisation);
	unsigned long long shortInterest = 0;
	for (const auto& kv : ob.agents) { if (kv.second != nullptr) { shortInterest += kv.second->shortShares; } }
	l.shortInterest = shortInterest;

	FrameHouse& h = frame.house;
	h.commissions = ob.ledger.commissions;
	h.exchangeFees = ob.ledger.exchangeFees;
	h.regulatoryFees = ob.ledger.regulatoryFees;
	h.marginInterest = ob.ledger.marginInterest;
	h.borrowFees = ob.ledger.borrowFees;
	h.brokerLosses = ob.ledger.brokerLosses;

	const BrokerStats& st = this->sim_.broker.stats;
	FrameBrokerCounts& b = frame.broker;
	b.marginCalls = st.marginCalls;
	b.writeOffs = st.writeOffs;
	b.stopsTriggered = st.stopsTriggered;
	b.cascades = st.cascadePumps;
	b.deepestCascade = st.maxStopsInOnePump;
	b.recalledShares = st.recalledShares;
	b.buyIns = st.buyInOrders;
	b.brackets = st.groupsCreated;
	b.tradingPauses = st.tradingPauses;
	b.ssrTriggers = ob.ssrTriggers;
}
