#include "include/CoreSim.h"
#include "include/Agent.h"
#include "include/Holding.h"
#include "include/Enums.h"
#include "include/Util.h"
#include "include/SimClock.h"
#include <algorithm>
#include <cmath>

namespace {

/* Hand out exactly poolShares among weights, largest remainder first
*
* Every agent's exact claim is poolShares * w / sum(w). Taking the floor of each always
* under-allocates, so the shortfall is handed out one share at a time to the largest
* fractional parts -- Hamilton apportionment. Two properties matter here:
*
*   * the pool is exhausted EXACTLY, at any population and any float, which is what stops
*     the float being partly imaginary the way the old stick-break left it below ~500 agents
*   * an agent whose exact claim is under one share still has a fair claim on the leftover
*     rather than being guaranteed nothing, so the small end of the tail is not
*     systematically zeroed
*
* Ties break on index so the result is deterministic for a given seed.
*/
void apportionShares(const std::vector<double>& weights, unsigned int poolShares,
	std::vector<unsigned int>& out) {

	out.assign(weights.size(), 0u);
	if (weights.empty() || poolShares == 0) { return; }

	double total = 0.0;
	for (double w : weights) { total += w; }

	// Degenerate only if every weight underflowed to zero. Falling back to equal claims
	// keeps the pool conserved instead of dividing by zero and losing the float.
	bool equalClaims = !(total > 0.0);
	if (equalClaims) { total = double(weights.size()); }

	std::vector<std::pair<double, size_t>> remainders;
	remainders.reserve(weights.size());

	unsigned int assigned = 0;
	for (size_t i = 0; i < weights.size(); ++i) {
		double exact = double(poolShares) * (equalClaims ? 1.0 : weights[i]) / total;
		double whole = std::floor(exact);
		out[i] = (unsigned int)whole;
		assigned += out[i];
		remainders.emplace_back(exact - whole, i);
	}

	unsigned int leftover = (poolShares > assigned) ? (poolShares - assigned) : 0u;
	if (leftover == 0) { return; }
	if (leftover > remainders.size()) { leftover = (unsigned int)remainders.size(); }

	std::sort(remainders.begin(), remainders.end(),
		[](const std::pair<double, size_t>& a, const std::pair<double, size_t>& b) {
			if (a.first != b.first) { return a.first > b.first; }
			return a.second < b.second;
		});

	for (unsigned int k = 0; k < leftover; ++k) { ++out[remainders[k].second]; }
}

} // namespace

// ---- Main Simulation Loop ----

void CoreSim::run(SimClock& clock) {
	// Clear stale state from any previous run
	while (!this->eventCallQueue.empty()) this->eventCallQueue.pop();
	this->isRunning = false;
	this->cancelRequested.store(false);
	clock.reset();
	
	this->OB.resetToInitial(this->parameters.obStartPrice, this->parameters.obShareFloat, true);  // = OrderBook(clock, this->parameters.obStartPrice, this->parameters.obShareFloat);
	this->OB.clock = &clock;

	// The agents map was just cleared, so every pooled slot id in the free list is dangling
	this->transientFreeList.clear();
	this->liveTransientCount = 0;
	this->transientSlotsAllocated = 0;
	this->transientArrivals = 0;
	this->transientDepartures = 0;
	this->transientPooled = 0;
	this->transientStranded = 0;
	this->lastArrivalCheckMs = 0.0;
	this->residentCount = 0;
	this->runTransientFraction = 0.0;
	this->cashScale = 1.0;

	//std::cout << "Initializing Agents..." << std::endl;
	this->initAgents(this->parameters.agentStartCount);

	//std::cout << "Running Back Data..." << std::endl;
	// Headless, unthrottled, bounded by simulated time. Leaves the clock sitting on
	// the open of the configured live start session with history already built.
	this->runBackData(clock);

	if (this->backDataAborted) {
		this->isRunning = false;
		return;
	}

	clock.start();
	clock.resume(); // recalibrate the wall clock against the non-zero handoff time
	this->isRunning = true;
	this->shouldGetSnapshot = false;
	long long lastTick = 0;
	int quietSlices = 0;

	while (this->isRunning) {
		
		// Handle Pause
		while (clock.paused.load() && !clock.step.load()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}

		// A quiet market is not a finished one. With nobody taking part, let the
		// clock run on until a sweep or session change brings traders back.
		if (this->eventCallQueue.empty()) {
			if (this->OB.agents.empty()) {
				this->isRunning = false;
				break;
			}

			// Follow the wall clock in short slices rather than jumping ahead and
			// sleeping out the gap, so pause, speed and quit stay responsive while
			// a dead session passes
			std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(PACING_SLICE_MS));

			double quietTarget = clock.simTargetMs();
			if (quietTarget > clock.simTimeMs) { clock.simTimeMs = quietTarget; }

			this->processSessionBoundaries(clock.simTimeMs, clock);
			if (clock.simTimeMs >= this->nextParticipationSweepMs) {
				this->sweepParticipation(clock.simTimeMs);
			}

			// Keep the clock and session readouts moving even with no trades
			if (++quietSlices >= QUIET_TICKS_PER_REFRESH) {
				quietSlices = 0;
				if (this->onTick) { this->onTick(); }
			}
			continue;
		}

		// Cross any session boundaries the next event would jump over, before it fires.
		// Read the call time first, an overnight skip rebuilds the queue underneath us.
		double nextCallTime = this->eventCallQueue.top().callTime;
		if (nextCallTime >= this->nextBoundaryMs) {
			if (this->processSessionBoundaries(nextCallTime, clock)) { continue; }
		}

		// Refresh who is in the market, participation drifts continuously off hours
		if (nextCallTime >= this->nextParticipationSweepMs) {
			this->sweepParticipation(nextCallTime);
		}

		// By value, NOT by reference. This call is still read after the pop below, and
		// pop() swaps the top element to the back before dropping it, so a reference to
		// top() would name the NEXT event afterwards. Binding a reference here set the
		// clock to the following event's time, one pacing had not waited for yet, which
		// ran the live sim ~25% fast at 1x. The back-data pump avoids this by copying
		// the call time into a local before popping.
		const EventCall nextEventCall = this->eventCallQueue.top();

		std::shared_ptr<Agent> agent = this->OB.getAgent(nextEventCall.agentId);

		// Discard calls left behind when an agent was woken early
		if (agent == nullptr || nextEventCall.generation != agent->eventGeneration) {
			this->eventCallQueue.pop();
			continue;
		}

		// Pace sim with wall clock, sleep if sim is ahead. Sliced rather than one
		// long block so a sparse session cannot lock out pause, speed or quit.
		bool waitInterrupted = false;
		while (this->isRunning) {
			double simTarget = clock.simTargetMs();
			if (nextEventCall.callTime <= simTarget) { break; }
			if (clock.paused.load()) { waitInterrupted = true; break; }

			double sleepMs = (nextEventCall.callTime - simTarget) / clock.speedMultiplier.load();
			if (sleepMs > PACING_SLICE_MS) { sleepMs = PACING_SLICE_MS; }
			std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(sleepMs));
		}
		// Re-enter the loop so the pause handler runs and the queue is re-read
		if (waitInterrupted || !this->isRunning) { continue; }

		// Process Event
		this->eventCallQueue.pop();
		agent->hasPendingEvent = false;
		clock.simTimeMs = nextEventCall.callTime;

		// Agent has stepped out of the market, drop it until a sweep brings it back.
		// shouldAct, not isParticipating: an agent on its way out keeps acting until flat.
		if (!agent->shouldAct(this->OB.session, clock.simTimeMs)) { continue; }

		agent->actRandom();

		// Before rescheduling, so a slot that just went back to the pool is not given an
		// event it would only discard
		this->updateTransientLifecycle(agent, clock.simTimeMs);
		if (agent->status != AgentStatus::POOLED) {
			this->scheduleNextEventCall(agent, clock.simTimeMs);
		}
		this->drainWakeQueue(clock.simTimeMs);
		if (this->onLog) { this->onLog({ LogEntry::Kind::HOLD, clock.simTimeMs, agent->id }); }

		// Handle step mode
		if (clock.step.load()) {
			clock.step.store(false);
			clock.pause();
		}

		// Redundant
		if (this->OB.tickCount != lastTick) {
			this->shouldGetSnapshot = true;
		}

		// Capture Snapshot, UI update
		if (this->shouldGetSnapshot) {
			//Snapshot snap = this->OB.getSnapshot();
			if (this->onTick) { onTick(); }

			this->shouldGetSnapshot = false;
			lastTick = this->OB.tickCount;
		}
		
		// Add random retail agents
		//if (market event or random chance) {
		//	std::shared_ptr<Agent> a_ = std::make_shared<Agent>(this->OB.makeId(ID_TYPE::AGENT), randomDouble(100.00, 3'600.00), randomDouble(100.00, 1000.00), AgentStatus::ACTIVE, AgentType::RETAIL, AgentSubType::NOISE, this->OB, this->ME);
		//	this->OB.upsertAgent(a_);
		//	scheduleNextEventCall(a_, clock.simTimeMs);
		//}
		
	}
}

// ---- Simulation Initialization Functions ----

void CoreSim::initAgents(unsigned int _agentStartCount) {
	// 0 asks for a population sized to the market rather than set by hand. Resolved here
	// rather than in run(), so every caller gets it -- a test driving initAgents directly
	// would otherwise build an empty market and divide by zero below.
	if (_agentStartCount == 0) {
		_agentStartCount = derivedPopulation(this->OB.currentPrice, this->OB.shareFloat);
	}

	// Everything this function creates is resident. The arrival rate scales against this
	// count, so transient agents never breed more transient agents.
	this->residentCount = _agentStartCount;

	// Ownership structure for this run. Drawn rather than fixed, see INST_FLOAT_SHARE_MEDIAN:
	// a constant here made every market identical in the one respect that most decides who
	// can supply the sell side.
	double instFloatShare = randomDouble(
		INST_FLOAT_SHARE_MEDIAN * (1.0 - INST_FLOAT_SHARE_SPREAD),
		INST_FLOAT_SHARE_MEDIAN * (1.0 + INST_FLOAT_SHARE_SPREAD));

	// Agent Type probabilities
	// This should scale with stock cap (micro -> higher retail probability, Large cap -> more institution still high retail)
	double percRetail = randomDouble(PERC_RETAIL_MIN, PERC_RETAIL_MAX);

	// ---- Cash scale ----
	//
	// The cash bands are a shape in unit dollars; this converts them into money for a
	// market of this size. Drawn from the EXPECTED type mix, which is known here because
	// percRetail has just been drawn -- the realised mix varies around it, so total cash
	// varies around the target rather than landing on it exactly, which is wanted: not
	// every market carries identical dry powder.
	//
	// Reads OB.currentPrice and OB.shareFloat rather than the parameters, since
	// resetToInitial is what actually settles them (a shareFloat of 0 is randomised there).
	double unitMeanPerAgent = percRetail * retailUnitMeanCash()
		+ (1.0 - percRetail) * instUnitMeanCash();
	double targetCash = CASH_TO_MARKET_CAP * this->OB.currentPrice * double(this->OB.shareFloat);
	this->cashScale = (_agentStartCount > 0 && unitMeanPerAgent > 0.0)
		? targetCash / (double(_agentStartCount) * unitMeanPerAgent)
		: 1.0;

	// Retail Agent SubType probabilities
	double noiseType_R = randomDouble(0.30, 0.50);
	double momentumType_R = randomDouble(noiseType_R+0.1, 0.88);
	double algoType_R = randomDouble(momentumType_R+0.1, 0.98);

	// Insitutional Agent SubType probabilities
	double algoType_I = randomDouble(0.50, 0.95);
	double informedType_I = randomDouble(algoType_I, 1.00);

	// Agent parameters
	double a_reactionTimeFloor;
	double a_accountCash;
	AgentStatus a_status = AgentStatus::ACTIVE;
	AgentType a_type;
	AgentSubType a_subType;
	double a_weight;

	double roll;

	// Shares cannot be handed out inside the loop: apportioning a pool needs every weight
	// in it, and no weight exists until the last agent has been built. So the loop draws
	// weights, and the float is dispersed against them afterwards.
	std::vector<std::shared_ptr<Agent>> instAgents, retailAgents;
	std::vector<double> instWeights, retailWeights;
	instAgents.reserve(_agentStartCount);
	retailAgents.reserve(_agentStartCount);
	instWeights.reserve(_agentStartCount);
	retailWeights.reserve(_agentStartCount);

	// for each iteration
	for (unsigned int i = 0; i < _agentStartCount; ++i) {
		// Agent Type
		a_type = randomDouble(0.0, 1.0) <= percRetail ? AgentType::RETAIL : AgentType::INSTITUTION;
		
		// Agent SubType
		roll = randomDouble(0.0, 1.0);
		if (a_type == AgentType::RETAIL) {

			// Agent SubTypes
			if (roll <= noiseType_R) { a_subType = AgentSubType::NOISE; }
			else if (roll > noiseType_R && roll <= momentumType_R) { a_subType = AgentSubType::MOMENTUM; }
			else if (roll > momentumType_R && roll <= algoType_R) { a_subType = AgentSubType::ALGO; }
			else { a_subType = AgentSubType::INFORMED; }

			// Agent Reaction Times
			//																	 0.2s        1hr
			if (a_subType == AgentSubType::NOISE) { a_reactionTimeFloor = randomDouble(200.0, 3'600'000.0); }
			//																			  0.5s     15min
			else if (a_subType == AgentSubType::MOMENTUM) { a_reactionTimeFloor = randomDouble(5000.0, 900'000.0); }
			//																		  0.01s    2s
			else if (a_subType == AgentSubType::ALGO) { a_reactionTimeFloor = randomDouble(10.0, 2000.0); }
			//									 0.2s        1hr
			else { a_reactionTimeFloor = randomDouble(200.0, 3'600'000.0); }

			// Agent Account Balances
			// Lower account balances 75% more likely. Scaled AFTER the draw, so the RNG
			// stream and the wealth/subtype correlation are both exactly as before.
			a_accountCash = this->cashScale * (roll <= RETAIL_CASH_SMALL_SHARE
				? randomDouble(RETAIL_CASH_SMALL_MIN, RETAIL_CASH_SMALL_MAX)
				: randomDouble(RETAIL_CASH_LARGE_MIN, RETAIL_CASH_LARGE_MAX));

			// Claim on the retail pool. Normalised against the pool after the loop, so
			// only the SPREAD of these matters, never their scale.
			a_weight = std::exp(HOLDING_WEIGHT_SIGMA_RETAIL * sampleNormal());
		}
		else {
			a_subType = roll <= algoType_I ? AgentSubType::ALGO : AgentSubType::INFORMED;
			//															    0.001s, 0.1s			    0.2s    1hr
			a_reactionTimeFloor = a_subType == AgentSubType::ALGO ? randomDouble(0.1, 1.0) : randomDouble(200.0, 3'600'000.0);
			a_accountCash = this->cashScale * randomDouble(INST_CASH_MIN, INST_CASH_MAX);

			// Claim on the institutional pool, tighter than retail: institutional
			// positions cluster harder than a retail register's long tail.
			a_weight = std::exp(HOLDING_WEIGHT_SIGMA_INST * sampleNormal());
		}

		// Create Agent
		std::shared_ptr<Agent> agent = std::make_shared<Agent>(
			this->OB.makeId(ID_TYPE::AGENT),
			a_reactionTimeFloor,
			a_accountCash,
			a_status,
			a_type,
			a_subType,
			this->OB,
			this->ME
		);

		// Passive quoting agents claim an early slot with their fast floor, then back
		// off to a re-quote cadence. A fill wakes them again, so a stale quote is
		// replaced on being hit rather than after waiting out the timer.
		//													  1s        60s
		if (a_subType == AgentSubType::ALGO) { agent->idleReactionTimeFloor = randomDouble(1000.0, 60'000.0); }

		// Upsert Agent
		this->OB.upsertAgent(agent);

		if (a_type == AgentType::INSTITUTION) {
			instAgents.push_back(agent);
			instWeights.push_back(a_weight);
		}
		else {
			retailAgents.push_back(agent);
			retailWeights.push_back(a_weight);
		}
	}

	// ---- Disperse the float ----
	//
	// Every share of the float ends up in someone's hands, at every population and every
	// float size. Agents whose claim rounds to nothing simply start flat, which is a real
	// kind of participant -- someone who has not bought in yet -- rather than a defect to
	// be patched.
	unsigned int instPool = (unsigned int)std::llround(double(this->OB.shareFloat) * instFloatShare);
	if (instPool > this->OB.shareFloat) { instPool = this->OB.shareFloat; }
	unsigned int retailPool = this->OB.shareFloat - instPool;

	// A run can legitimately draw a population with no institutions at all (percRetail
	// reaches 1.00). Folding the orphaned pool into the other one keeps the float whole;
	// leaving it unclaimed is what used to make shareFloat partly imaginary.
	if (instAgents.empty()) { retailPool += instPool; instPool = 0; }
	else if (retailAgents.empty()) { instPool += retailPool; retailPool = 0; }

	std::vector<unsigned int> instShares, retailShares;
	apportionShares(instWeights, instPool, instShares);
	apportionShares(retailWeights, retailPool, retailShares);

	for (size_t i = 0; i < instAgents.size(); ++i) { this->seedHolding(instAgents[i], instShares[i]); }
	for (size_t i = 0; i < retailAgents.size(); ++i) { this->seedHolding(retailAgents[i], retailShares[i]); }

	// Drawn last, once every resident exists. See rollRunTransientFraction.
	this->rollRunTransientFraction();
}
void CoreSim::seedHolding(std::shared_ptr<Agent> agent, unsigned int shares) {
	if (agent == nullptr || shares == 0) { return; }

	// Which side of the spread the position was notionally acquired on
	double roll = randomDouble(0.0, 1.0);
	OrderAction side = (roll <= 0.50) ? OrderAction::ASK : OrderAction::BID;

	// Cost basis only. Nothing reads Holding::price to make a decision; it exists so a
	// starting position looks like one built up over time rather than all bought at
	// today's price.
	double basisPrice = this->OB.currentPrice;
	roll = randomDouble(0.0, 1.0);
	if (roll > 0.50) {
		double factor = randomDouble(1.0, 10.0);

		// The "below" case is the multiplicative MIRROR of the "above" case. It used to
		// read currentPrice - (currentPrice * factor), which is currentPrice * (1 - factor)
		// and therefore <= 0 for every factor in [1, 10]. getMaxVariance then evaluated
		// pow(negative, -decayRate) and log(negative), so 23-27% of all starting lots
		// carried a NaN cost basis. It never moved the market, since no decision reads
		// this price -- but holdings is an unordered_map keyed BY price and NaN != NaN,
		// so try_emplace could never merge such a lot and an agent recycling one
		// accumulated duplicate entries without bound.
		basisPrice = (roll <= 0.75)
			? this->OB.currentPrice / (1.0 + factor)
			: this->OB.currentPrice + (this->OB.currentPrice * factor);
	}

	agent->upsertHolding(Holding(agent->getBetaPrice(basisPrice, side), shares));
}
void CoreSim::rollRunTransientFraction() {
	// Off is off, and draws nothing
	if (this->parameters.transientFraction <= 0.0) {
		this->runTransientFraction = 0.0;
		return;
	}

	// Lognormal about the configured median, so most runs sit near it and a thick or thin
	// market is occasional rather than routine. Clamped so a tail draw cannot produce one
	// that is absurd.
	double factor = std::exp(TRANSIENT_FRACTION_LOG_SD * sampleNormal());
	if (factor < 1.0 / TRANSIENT_FRACTION_MAX_FACTOR) { factor = 1.0 / TRANSIENT_FRACTION_MAX_FACTOR; }
	if (factor > TRANSIENT_FRACTION_MAX_FACTOR) { factor = TRANSIENT_FRACTION_MAX_FACTOR; }

	this->runTransientFraction = this->parameters.transientFraction * factor;
}
void CoreSim::reportBackDataProgress(const SimClock& clock, long long eventsProcessed) {
	if (!this->onBackDataProgress) { return; }

	BackDataProgress p;
	p.simTimeMs = clock.simTimeMs;
	p.session = this->OB.session;
	p.dayIndex = MarketCalendar::dayIndex(clock.simTimeMs);
	p.activeMinutes = MarketCalendar::activeMinutesElapsed(clock.simTimeMs);
	p.totalMinutes = MarketCalendar::msToMinutes(clock.simTimeMs);
	p.targetMinutes = MarketCalendar::msToMinutes(this->backDataTargetMs);
	p.events = eventsProcessed;
	p.ticks = (long long)this->OB.tickHistory.size();
	p.extraDays = this->backDataExtraDays;
	p.transientArrivals = this->transientArrivals;
	p.transientFraction = this->runTransientFraction;
	p.liveTransients = this->liveTransientCount;

	double pct = (this->backDataTargetMs > 0.0) ? (clock.simTimeMs / this->backDataTargetMs) * 100.0 : 100.0;
	p.percent = (pct < 0.0) ? 0.0 : (pct > 100.0 ? 100.0 : pct);

	this->onBackDataProgress(p);
}
bool CoreSim::pumpBackDataEvents(double targetMs, SimClock& clock, long long& eventsProcessed,
	const std::chrono::steady_clock::time_point& wallStart) {

	while (clock.simTimeMs < targetMs) {
		if (this->eventCallQueue.empty()) {
			if (this->OB.agents.empty()) { return true; }

			// Nobody is in the market right now, which is a legitimate state off
			// hours. Let time pass until a sweep or a session change brings
			// participants back rather than stalling or ending the run.
			double advanceTo = (std::min)(this->nextParticipationSweepMs, this->nextBoundaryMs);
			if (advanceTo <= clock.simTimeMs) {
				advanceTo = clock.simTimeMs + MarketCalendar::minutesToMs(PARTICIPATION_SWEEP_MINUTES);
			}
			if (advanceTo >= targetMs) { return true; }

			clock.simTimeMs = advanceTo;
			this->processSessionBoundaries(advanceTo, clock);
			this->sweepParticipation(advanceTo);
			continue;
		}

		// Read the call time before touching the queue, a skip rebuilds it underneath us
		double nextCallTime = this->eventCallQueue.top().callTime;

		// Stop cleanly at the target rather than overshooting it
		if (nextCallTime >= targetMs) { return true; }

		// Cross any session boundaries this event would jump over
		if (nextCallTime >= this->nextBoundaryMs) {
			if (this->processSessionBoundaries(nextCallTime, clock)) { continue; }
		}

		// Refresh who is in the market, participation drifts continuously off hours
		if (nextCallTime >= this->nextParticipationSweepMs) {
			this->sweepParticipation(nextCallTime);
		}

		const EventCall& nextEventCall = this->eventCallQueue.top();
		std::shared_ptr<Agent> agent = this->OB.getAgent(nextEventCall.agentId);

		// Discard calls left behind when an agent was woken early
		if (agent == nullptr || nextEventCall.generation != agent->eventGeneration) {
			this->eventCallQueue.pop();
			continue;
		}

		this->eventCallQueue.pop();
		agent->hasPendingEvent = false;
		clock.simTimeMs = nextCallTime;

		// Agent has stepped out of the market, drop it until a sweep brings it back.
		// shouldAct, not isParticipating: an agent on its way out keeps acting until flat.
		if (!agent->shouldAct(this->OB.session, clock.simTimeMs)) { continue; }

		agent->actRandom();

		this->updateTransientLifecycle(agent, clock.simTimeMs);
		if (agent->status != AgentStatus::POOLED) {
			this->scheduleNextEventCall(agent, clock.simTimeMs);
		}
		this->drainWakeQueue(clock.simTimeMs);
		++eventsProcessed;

		// Caps, cancellation and progress are checked on an interval, not per event
		if ((eventsProcessed % BACK_DATA_CHECK_INTERVAL) == 0) {
			if (this->cancelRequested.load()) { return false; }
			if (eventsProcessed >= BACK_DATA_MAX_EVENTS) { return false; }

			double wallSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();
			if (wallSeconds >= BACK_DATA_MAX_WALL_SECONDS) { return false; }

			this->reportBackDataProgress(clock, eventsProcessed);
		}
	}

	return true;
}
void CoreSim::runBackData(SimClock& clock) {
	double liveStartMs = this->parameters.backDataDurationMs();

	// Back data always begins at the day 0 premarket open
	clock.simTimeMs = 0.0;
	this->OB.session = MarketCalendar::sessionAt(0.0);
	this->nextBoundaryMs = MarketCalendar::nextBoundaryMs(0.0);
	this->backDataAborted = false;
	this->backDataExtraDays = 0;
	this->backDataTargetMs = liveStartMs;
	this->backDataRunning.store(true);
	this->nextParticipationSweepMs = 0.0;
	this->lastArrivalCheckMs = 0.0;

	// Seeds the queue with whoever is actually trading at the premarket open
	for (const auto& kv : this->OB.agents) { if (kv.second != nullptr) { kv.second->hasPendingEvent = false; } }
	this->sweepParticipation(0.0);

	long long eventsProcessed = 0;
	auto wallStart = std::chrono::steady_clock::now();
	this->reportBackDataProgress(clock, eventsProcessed);

	// ---- Main back-data span ----
	bool completed = true;
	if (liveStartMs > 0.0) {
		completed = this->pumpBackDataEvents(liveStartMs, clock, eventsProcessed, wallStart);
	}

	// ---- Optional extension to reach the minimum liquidity ----
	// Extends a whole day at a time so the handoff still lands on the same session open
	double handoffMs = liveStartMs;
	if (completed && this->parameters.minLiquidity > 0) {
		int extraDays = 0;
		while (extraDays < BACK_DATA_MAX_EXTRA_DAYS
			&& (this->OB.getNumBids() < int(this->parameters.minLiquidity)
				|| this->OB.getNumAsks() < int(this->parameters.minLiquidity))) {

			handoffMs += MarketCalendar::minutesToMs(MarketCalendar::TOTAL_MINUTES_PER_DAY);
			++extraDays;
			this->backDataExtraDays = extraDays;
			this->backDataTargetMs = handoffMs;
			completed = this->pumpBackDataEvents(handoffMs, clock, eventsProcessed, wallStart);
			if (!completed) { break; }
		}

		bool stillThin = this->OB.getNumBids() < int(this->parameters.minLiquidity)
			|| this->OB.getNumAsks() < int(this->parameters.minLiquidity);
		if (stillThin && this->onLog) {
			this->onLog({ LogEntry::Kind::HOLD, clock.simTimeMs,
				"WARNING: minimum liquidity not met after " + std::to_string(extraDays) + " extra days" });
		}
	}

	if (!completed) {
		this->backDataAborted = true;
		this->backDataRunning.store(false);
		if (this->onLog) {
			this->onLog({ LogEntry::Kind::HOLD, clock.simTimeMs, "BACK DATA ABORTED, cap reached or cancelled" });
		}
		return;
	}

	// ---- Handoff ----
	// Finish crossing any boundaries left between the final event and the handoff.
	// Loops because an overnight skip returns early having rebuilt the queue.
	while (this->processSessionBoundaries(handoffMs, clock)) {}

	// Land exactly on the live start and give every agent a fresh event from there,
	// so nothing fires in the past once the live loop takes over
	this->skipToTime(handoffMs, clock);
	this->OB.session = MarketCalendar::sessionAt(handoffMs);
	this->nextBoundaryMs = MarketCalendar::nextBoundaryMs(handoffMs);

	this->reportBackDataProgress(clock, eventsProcessed);
	this->backDataRunning.store(false);

	if (this->onLog) {
		EnumStrings es;
		this->onLog({ LogEntry::Kind::HOLD, clock.simTimeMs,
			"BACK DATA COMPLETE, " + std::to_string(this->OB.tickCount) + " ticks, opening in " + es.sessionString[this->OB.session] });
	}
}

// ---- Session Functions ----

bool CoreSim::processSessionBoundaries(double targetSimTimeMs, SimClock& clock) {
	bool queueRebuilt = false;

	while (targetSimTimeMs >= this->nextBoundaryMs) {
		double boundaryMs = this->nextBoundaryMs;
		Session endingSession = this->OB.session;

		// Expire resting orders when the closing session expires them.
		// PREMARKET is skipped, it rolls into REGULAR the way it does in real markets.
		if (MarketCalendar::expiresAtSessionEnd(endingSession)) {
			unsigned int expiredCount = this->OB.expireOrders(boundaryMs);
			if (expiredCount > 0 && this->onLog) {
				this->onLog({ LogEntry::Kind::CANCEL, boundaryMs, "EXPIRED " + std::to_string(expiredCount) + " orders" });
			}
		}

		this->OB.session = MarketCalendar::sessionAt(boundaryMs);
		if (this->onLog) {
			EnumStrings es;
			this->onLog({ LogEntry::Kind::HOLD, boundaryMs, "SESSION " + es.sessionString[this->OB.session] });
		}

		// Only a small share of overnight windows are worth simulating, the rest
		// are skipped so the clock lands straight on the next premarket open
		if (this->OB.session == Session::OVERNIGHT && randomDouble(0.0, 1.0) > OVERNIGHT_SIM_PROBABILITY) {
			double resumeAtMs = MarketCalendar::sessionEndMs(boundaryMs); // overnight ends at the next day's premarket open

			// Nobody transient sits through a night the sim did not simulate. Without this
			// they cross the gap intact and their recorded tenure runs past the hard cap by
			// the whole length of the skip.
			this->departAllTransients(boundaryMs);

			this->skipToTime(resumeAtMs, clock);
			queueRebuilt = true;

			this->OB.session = MarketCalendar::sessionAt(resumeAtMs);
			this->nextBoundaryMs = MarketCalendar::nextBoundaryMs(resumeAtMs);

			if (this->onLog) {
				EnumStrings es;
				this->onLog({ LogEntry::Kind::HOLD, resumeAtMs, "SKIPPED OVERNIGHT, SESSION " + es.sessionString[this->OB.session] });
			}

			// The queue now holds freshly scheduled events, the caller must re-read it
			return queueRebuilt;
		}

		this->nextBoundaryMs = MarketCalendar::nextBoundaryMs(boundaryMs);

		// The population changes shape at every session change
		this->sweepParticipation(boundaryMs);
	}

	return queueRebuilt;
}
void CoreSim::skipToTime(double resumeAtMs, SimClock& clock) {
	clock.simTimeMs = resumeAtMs;

	// The clock just jumped, in the live loop by as much as a whole overnight.
	// Without rebasing, pacing sees the sim as hours ahead and sleeps out the gap.
	clock.rebaseWallClock();

	// Drop events left sitting inside the skipped window, they would otherwise
	// fire immediately and out of order on the far side of the jump
	std::priority_queue<EventCall, std::vector<EventCall>, CompareEventCalls> emptyQueue;
	std::swap(this->eventCallQueue, emptyQueue);
	this->OB.wakeQueue.clear(); // wakes are meaningless across a jump, everyone is rescheduled

	// The queue was emptied, so nobody holds a live event any more
	for (const auto& kv : this->OB.agents) { if (kv.second != nullptr) { kv.second->hasPendingEvent = false; } }

	// The clock jumped over a window in which no market ran, so no arrivals happened in it
	// either. Without this the whole skipped span's worth would be drawn and dumped at the
	// far side, landing an entire overnight's arrivals on the premarket open at once.
	this->lastArrivalCheckMs = resumeAtMs;

	// Only agents taking part in the session we land in get scheduled
	this->OB.session = MarketCalendar::sessionAt(resumeAtMs);
	this->sweepParticipation(resumeAtMs);
}

// ---- Event Functions ----

void CoreSim::scheduleNextEventCall(std::shared_ptr<Agent> agent, double simTime) {
	// Passive quoting agents keep their fast floor for the opening action only, so they
	// still claim an early slot and seed the book, then back off to a re-quote cadence.
	// Applied unconditionally after that first action rather than while at order
	// capacity, which would spin an agent that has a free side but cannot use it.
	// reactionTimeFloor itself is never overwritten, wakes still need the fast value.
	double floorMs = (agent->idleReactionTimeFloor > 0.0 && agent->actionCount > 0)
		? agent->idleReactionTimeFloor
		: agent->reactionTimeFloor;

	double jitter = randomDouble(0.0, floorMs);
	agent->reactionTime = floorMs + jitter;  // used for sentiment calculation
	double nextEventCallTime = simTime + agent->reactionTime;

	agent->eventGeneration++;
	agent->hasPendingEvent = true;
	EventCall ec = EventCall(nextEventCallTime, agent->id, agent->eventGeneration);
	this->eventCallQueue.push(ec);
}
void CoreSim::sweepParticipation(double simTimeMs) {
	for (const auto& kv : this->OB.agents) {
		std::shared_ptr<Agent> agent = kv.second;
		if (agent == nullptr) { continue; }

		// A pooled slot is not a participant. It is an unoccupied agent object waiting to
		// be rerolled, and must never be given a session state or an event -- during
		// REGULAR the participation rate is 1.0, so treating one as merely INACTIVE would
		// schedule the entire pool and silently double the population.
		if (agent->status == AgentStatus::POOLED) { continue; }

		// The departure hazard runs here as well as after an action, so a transient agent
		// that has gone dormant off hours still leaves instead of holding its slot forever.
		// Safe to call while iterating: nothing here touches the agents map.
		this->updateTransientLifecycle(agent, simTimeMs);
		if (agent->status == AgentStatus::POOLED) { continue; }

		bool participating = agent->isParticipating(this->OB.session, simTimeMs);

		// Lifecycle states outrank session states and are never overwritten by one
		if (!isLifecycleStatus(agent->status)) {
			agent->status = participating ? AgentStatus::ACTIVE : AgentStatus::INACTIVE;
		}

		// Agents that have just joined need an event, ones that dropped out simply
		// stop being rescheduled and fall out of the queue on their own. An agent that is
		// leaving is scheduled regardless of gating, or it could never finish unwinding.
		if (agent->shouldAct(this->OB.session, simTimeMs) && !agent->hasPendingEvent) {
			this->scheduleNextEventCall(agent, simTimeMs);
		}
	}

	// Admit this interval's arrivals after the existing population has been refreshed, so
	// a brand new arrival is not immediately re-examined by the loop above
	this->processTransientArrivals(simTimeMs);

	this->nextParticipationSweepMs = simTimeMs + MarketCalendar::minutesToMs(PARTICIPATION_SWEEP_MINUTES);
}
// ---- Transient Agent Functions ----

/* Share of the REGULAR arrival rate this session sees */
static double transientSessionFactor(Session session) {
	switch (session) {
	case Session::PREMARKET:  return TRANSIENT_SESSION_FACTOR_PREMARKET;
	case Session::REGULAR:    return TRANSIENT_SESSION_FACTOR_REGULAR;
	case Session::AFTERHOURS: return TRANSIENT_SESSION_FACTOR_AFTERHOURS;
	case Session::OVERNIGHT:  return TRANSIENT_SESSION_FACTOR_OVERNIGHT;
	default:                  return 0.0;   // CLOSED, nobody arrives
	}
}

int CoreSim::transientPopulationCap() const {
	// Keep the ceiling clear of THIS RUN's fraction, not the configured median, so a run
	// that drew a busy market is not quietly clipped back toward an average one
	double capFraction = (std::max)(TRANSIENT_MAX_POPULATION_FRACTION,
		this->runTransientFraction * TRANSIENT_CAP_HEADROOM);
	return int(capFraction * double(this->residentCount));
}
void CoreSim::processTransientArrivals(double simTimeMs) {
	// The disabled path must consume no RNG at all, so that a run with transient agents
	// off reproduces a pre-feature run exactly. Every early return here is before a draw.
	if (this->runTransientFraction <= 0.0) {
		this->lastArrivalCheckMs = simTimeMs;
		return;
	}
	if (this->residentCount <= 0) {
		this->lastArrivalCheckMs = simTimeMs;
		return;
	}

	double dtMs = simTimeMs - this->lastArrivalCheckMs;
	this->lastArrivalCheckMs = simTimeMs;
	if (dtMs <= 0.0) { return; }

	// Little's law: to hold `fraction` of the resident count in the market at once, given a
	// mean stay of meanTenureHours, arrivals must run at population / meanTenureHours.
	double meanTenureHours = TRANSIENT_MEAN_TENURE_MINUTES / 60.0;
	double targetPopulation = this->runTransientFraction * double(this->residentCount);
	double arrivalsPerHour = (targetPopulation / meanTenureHours) * transientSessionFactor(this->OB.session);
	if (arrivalsPerHour <= 0.0) { return; }

	double dtHours = MarketCalendar::msToMinutes(dtMs) / 60.0;
	unsigned int arrivals = samplePoisson(arrivalsPerHour * dtHours);
	if (arrivals == 0) { return; }

	// Excess is dropped rather than queued. The cap is a safety limit, and carrying a
	// backlog forward would turn one capped interval into a burst later.
	int cap = this->transientPopulationCap();
	for (unsigned int i = 0; i < arrivals; ++i) {
		if (this->liveTransientCount >= cap) { break; }

		std::shared_ptr<Agent> agent = this->acquireTransientSlot(simTimeMs);
		if (agent == nullptr) { break; }

		++this->liveTransientCount;
		++this->transientArrivals;
		this->scheduleNextEventCall(agent, simTimeMs);
	}
}

void CoreSim::updateTransientLifecycle(std::shared_ptr<Agent> agent, double simTimeMs) {
	if (agent == nullptr || !agent->isTransient) { return; }
	if (agent->status == AgentStatus::POOLED) { return; }

	// The agent's own order maps are the cheap view and are kept in lockstep with the book.
	// OrderBook::agentHasRestingOrders is the authoritative one and is used where it matters,
	// on reroll -- running it here would walk the whole book for every transient on every
	// sweep, to answer a question the agent already knows the answer to.
	bool clean = agent->holdings.empty() && agent->activeBids.empty() && agent->activeAsks.empty();

	// A transient agent with nothing left is finished, whether it chose to leave or went
	// broke. BANKRUPT is only ever set with no cash, no holdings and no orders, so a
	// bankrupt transient is clean by definition and its slot is free to reuse.
	if (agent->status == AgentStatus::LEAVING || agent->status == AgentStatus::BANKRUPT) {
		if (clean) { this->poolTransientSlot(agent); return; }

		// Could not get flat inside the grace window. It is NOT retired holding its shares:
		// it stays in the market on a long horizon, still working the position off, so the
		// float keeps circulating. Jittered upward only, which keeps the floor strictly
		// above the stranded threshold and makes this a one-time stretch.
		double strandedFloorMs = MarketCalendar::minutesToMs(TRANSIENT_STRANDED_REACTION_MINUTES);
		if (agent->isStranded(simTimeMs) && agent->reactionTimeFloor < strandedFloorMs) {
			agent->reactionTimeFloor = strandedFloorMs * randomDouble(1.0, 1.5);
			++this->transientStranded;
		}
		return;
	}

	// The commitment window is what stops the tenure distribution collapsing onto zero
	if (simTimeMs < agent->minTenureEndsMs) { return; }

	double dtMs = simTimeMs - agent->lastDepartCheckMs;
	agent->lastDepartCheckMs = simTimeMs;
	if (dtMs <= 0.0) { return; }

	if (simTimeMs - agent->arrivedAtMs >= MarketCalendar::minutesToMs(TRANSIENT_MAX_TENURE_MINUTES)) {
		this->beginTransientDeparture(agent, simTimeMs);
		return;
	}

	// Hazard over elapsed SIM TIME, so an agent checking every two seconds and one checking
	// every fifteen minutes draw from the same tenure distribution. Adversity shortens the
	// time constant; a favourable market does not lengthen it.
	double tau = (agent->tenureHalfLifeMs / 0.693147180559945309417)
		/ (1.0 + TRANSIENT_ADVERSITY_GAIN * agent->adversity());
	if (tau <= 0.0) { this->beginTransientDeparture(agent, simTimeMs); return; }

	double pDepart = 1.0 - std::exp(-dtMs / tau);
	if (randomDouble(0.0, 1.0) < pDepart) { this->beginTransientDeparture(agent, simTimeMs); }
}
void CoreSim::beginTransientDeparture(std::shared_ptr<Agent> agent, double simTimeMs) {
	if (agent == nullptr || agent->status == AgentStatus::LEAVING
		|| agent->status == AgentStatus::POOLED) {
		return;
	}

	agent->status = AgentStatus::LEAVING;
	agent->leavingSinceMs = simTimeMs;
	++this->transientDepartures;

	// Reported before anything is unwound, while tenure and action count still describe the
	// life that just ended
	if (this->onTransientDepart) { this->onTransientDepart(agent, simTimeMs); }

	// Stop adding to the position. Collect first: cancelOrder erases from the very map
	// being iterated.
	OrderAction entry = agent->entrySide();
	std::vector<std::shared_ptr<Order>> toCancel;
	const auto& entryOrders = (entry == OrderAction::BID) ? agent->activeBids : agent->activeAsks;
	toCancel.reserve(entryOrders.size());
	for (const auto& kv : entryOrders) { toCancel.push_back(kv.second); }
	for (const std::shared_ptr<Order>& order : toCancel) { this->OB.cancelOrder(order, agent); }

	// An agent that arrived, never filled, and left is already clean
	if (agent->holdings.empty() && agent->activeBids.empty() && agent->activeAsks.empty()) {
		this->poolTransientSlot(agent);
	}
}
void CoreSim::departAllTransients(double simTimeMs) {
	if (this->liveTransientCount <= 0) { return; }

	// Safe to iterate: beginTransientDeparture only touches order queues and the free list
	for (const auto& kv : this->OB.agents) {
		std::shared_ptr<Agent> agent = kv.second;
		if (agent == nullptr || !agent->isTransient) { continue; }
		if (agent->status == AgentStatus::POOLED || agent->status == AgentStatus::LEAVING) { continue; }
		this->beginTransientDeparture(agent, simTimeMs);
	}
}
void CoreSim::poolTransientSlot(std::shared_ptr<Agent> agent) {
	if (agent == nullptr || agent->status == AgentStatus::POOLED) { return; }

	agent->status = AgentStatus::POOLED;
	agent->leavingSinceMs = 0.0;
	agent->hasPendingEvent = false;

	// Bump so any event already queued against this slot is discarded on pop. An empty seat
	// must not act, and rollTransientPersonality bumps again when the seat is refilled.
	agent->eventGeneration++;

	// A wake is meaningless for a slot nobody is sitting in
	this->OB.wakeQueue.erase(
		std::remove(this->OB.wakeQueue.begin(), this->OB.wakeQueue.end(), agent->id),
		this->OB.wakeQueue.end());

	if (this->liveTransientCount > 0) { --this->liveTransientCount; }
	++this->transientPooled;
	this->transientFreeList.push_back(agent->id);
}

bool CoreSim::rollTransientPersonality(std::shared_ptr<Agent> agent, double simTimeMs) {
	if (agent == nullptr) { return false; }

	// The slot must be clean. This is checked, never fixed: silently clearing holdings
	// would destroy shares and silently dropping orders would strand their escrow, and
	// either way it would hide the real bug, which is a slot pooled before it was flat.
	if (!agent->holdings.empty() || !agent->activeBids.empty() || !agent->activeAsks.empty()
		|| this->OB.agentHasRestingOrders(agent->id)) {
		if (this->onLog) {
			this->onLog({ LogEntry::Kind::HOLD, simTimeMs,
				"REFUSED to reroll unclean transient slot " + agent->id });
		}
		return false;
	}

	agent->isTransient = true;
	agent->incarnation++;

	// Long biased, like every agent in the sim today. A future SHORT subtype sets this to
	// -1 and everything below signs itself off it, arrival sentiment included.
	agent->directionalBias = 1.0;

	// Subtype, cumulative thresholds over one draw. ALGO is deliberately absent.
	double roll = randomDouble(0.0, 1.0);
	if (roll < TRANSIENT_MIX_NOISE) { agent->subType = AgentSubType::NOISE; }
	else if (roll < TRANSIENT_MIX_NOISE + TRANSIENT_MIX_MOMENTUM) { agent->subType = AgentSubType::MOMENTUM; }
	else { agent->subType = AgentSubType::INFORMED; }

	agent->type = AgentType::RETAIL;

	switch (agent->subType) {
	case AgentSubType::MOMENTUM:
		agent->reactionTimeFloor = randomDouble(TRANSIENT_REACTION_MIN_MOMENTUM_MS, TRANSIENT_REACTION_MAX_MOMENTUM_MS);
		break;
	case AgentSubType::INFORMED:
		agent->reactionTimeFloor = randomDouble(TRANSIENT_REACTION_MIN_INFORMED_MS, TRANSIENT_REACTION_MAX_INFORMED_MS);
		break;
	default:
		agent->reactionTimeFloor = randomDouble(TRANSIENT_REACTION_MIN_NOISE_MS, TRANSIENT_REACTION_MAX_NOISE_MS);
		break;
	}
	agent->reactionTime = agent->reactionTimeFloor;
	agent->idleReactionTimeFloor = 0.0;  // no ALGO backoff, transient agents are takers

	// Same scale as the residents. Transient agents arrive FLAT, which is settled and not
	// reopened here -- but their cash is money, and money has to be denominated in the same
	// market the residents live in or a transient is either a whale or an irrelevance
	// depending only on how big the float happens to be.
	agent->cash = this->cashScale * randomDouble(TRANSIENT_CASH_MIN, TRANSIENT_CASH_MAX);

	// Arrives holding conviction, not neutral: it turned up wanting to trade. OU then
	// pulls this back toward the market's neutral level over its tenure, which is what
	// makes "arrived keen, conviction faded, left" emerge rather than being scripted.
	agent->sentiment = randomDouble(TRANSIENT_ARRIVAL_SENTIMENT_MIN, TRANSIENT_ARRIVAL_SENTIMENT_MAX)
		* agent->directionalBias;
	agent->rollSentimentProcess();
	agent->sentimentEwma = agent->sentiment;

	// Drawn from [0, rate) for the session it is arriving into, so it is always below the
	// rate and therefore participating on arrival. An agent that showed up during the
	// premarket is by definition one of the people who trade the premarket. It can still
	// drop out later as the rate decays.
	double rate = agent->participationRate(this->OB.session, simTimeMs);
	agent->participationThreshold = randomDouble(0.0, rate);

	double halfLifeMinutes = TRANSIENT_TENURE_HALFLIFE_MINUTES
		* randomDouble(1.0 - TRANSIENT_TENURE_HALFLIFE_SPREAD, 1.0 + TRANSIENT_TENURE_HALFLIFE_SPREAD);
	agent->tenureHalfLifeMs = MarketCalendar::minutesToMs(halfLifeMinutes);
	agent->arrivedAtMs = simTimeMs;
	agent->minTenureEndsMs = simTimeMs + MarketCalendar::minutesToMs(TRANSIENT_MIN_TENURE_MINUTES);
	// The hazard integrates from the end of the commitment window, not from arrival
	agent->lastDepartCheckMs = agent->minTenureEndsMs;
	agent->leavingSinceMs = 0.0;

	agent->actionCount = 0;
	agent->status = AgentStatus::ACTIVE;

	// Bump, never reset: an event from the previous incarnation may still be queued, and
	// resetting would let it alias a live one instead of being discarded as stale.
	agent->eventGeneration++;
	agent->hasPendingEvent = false;

	return true;
}
std::shared_ptr<Agent> CoreSim::acquireTransientSlot(double simTimeMs) {
	// Prefer reviving a pooled slot over allocating. Only when the pool is empty does the
	// agents map grow, so it settles at the peak concurrent transient population.
	while (!this->transientFreeList.empty()) {
		std::string slotId = this->transientFreeList.back();
		this->transientFreeList.pop_back();

		std::shared_ptr<Agent> slot = this->OB.getAgent(slotId);
		if (slot == nullptr) { continue; }

		// A slot that is not clean should never have been pooled. Drop it rather than
		// returning it to the free list, and fall through to allocating instead, so one
		// bad slot cannot stall arrivals.
		if (this->rollTransientPersonality(slot, simTimeMs)) { return slot; }
	}

	std::shared_ptr<Agent> agent = std::make_shared<Agent>(
		this->OB.makeId(ID_TYPE::AGENT),
		1000.0,                  // placeholder, rerolled below
		0.0,                     // placeholder, rerolled below
		AgentStatus::ACTIVE,
		AgentType::RETAIL,
		AgentSubType::NOISE,
		this->OB,
		this->ME
	);

	if (!this->rollTransientPersonality(agent, simTimeMs)) { return nullptr; }

	this->OB.upsertAgent(agent);
	++this->transientSlotsAllocated;
	return agent;
}

void CoreSim::drainWakeQueue(double simTimeMs) {
	if (this->OB.wakeQueue.empty()) { return; }

	for (const std::string& agentId : this->OB.wakeQueue) {
		auto found = this->OB.agents.find(agentId);
		if (found == this->OB.agents.end()) { continue; }

		std::shared_ptr<Agent> agent = found->second;

		// A dormant agent stays dormant, it is not in the market right now
		if (!agent->isParticipating(this->OB.session, simTimeMs)) { continue; }

		// Act on the fill at the agent's fast floor rather than its idle cadence.
		// Bumping the generation leaves the previously scheduled call stale.
		double jitter = randomDouble(0.0, agent->reactionTimeFloor);
		agent->reactionTime = agent->reactionTimeFloor + jitter;

		agent->eventGeneration++;
		agent->hasPendingEvent = true;
		this->eventCallQueue.push(EventCall(simTimeMs + agent->reactionTime, agent->id, agent->eventGeneration));
	}

	this->OB.wakeQueue.clear();
}

// ---- Utility Functions ----

void CoreSim::setParameters(unsigned int seed, unsigned int backDataDays, Session liveStartSession, unsigned int minLiquidity, unsigned int agentStartCount, unsigned int obShareFloat, double obStartPrice, double transientFraction) {

	this->parameters.seed = seed;
	this->parameters.backDataDays = backDataDays;
	this->parameters.liveStartSession = liveStartSession;
	this->parameters.minLiquidity = minLiquidity;
	this->parameters.agentStartCount = agentStartCount;
	this->parameters.obShareFloat = obShareFloat;
	this->parameters.obStartPrice = obStartPrice;
	this->parameters.transientFraction = transientFraction;

	setSeed(this->parameters.seed);
}