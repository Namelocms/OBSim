#include "include/CoreSim.h"
#include "include/Agent.h"
#include "include/Holding.h"
#include "include/Enums.h"
#include "include/Util.h"
#include "include/SimClock.h"

// ---- Main Simulation Loop ----

void CoreSim::run(SimClock& clock) {
	// Clear stale state from any previous run
	while (!this->eventCallQueue.empty()) this->eventCallQueue.pop();
	this->isRunning = false;
	this->cancelRequested.store(false);
	clock.reset();
	
	this->OB.resetToInitial(this->parameters.obStartPrice, this->parameters.obShareFloat, true);  // = OrderBook(clock, this->parameters.obStartPrice, this->parameters.obShareFloat);
	this->OB.clock = &clock;

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

		const EventCall& nextEventCall = this->eventCallQueue.top();

		std::shared_ptr<Agent> agent = this->OB.agents[nextEventCall.agentId];

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

		// Agent has stepped out of the market, drop it until a sweep brings it back
		if (!agent->isParticipating(this->OB.session, clock.simTimeMs)) { continue; }

		agent->actRandom();
		this->scheduleNextEventCall(agent, clock.simTimeMs);
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

void CoreSim::initAgents(unsigned short _agentStartCount) {
	// track current share count against float
	unsigned int dispersedShares_I = unsigned int(this->OB.shareFloat * 0.70);
	unsigned int dispersedShares_R = this->OB.shareFloat - dispersedShares_I;

	// Agent Type probabilities
	// This should scale with stock cap (micro -> higher retail probability, Large cap -> more institution still high retail)
	double percRetail = randomDouble(0.70, 1.00);

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
	unsigned int startingShares;

	double roll;

	// for each iteration
	for (unsigned short i = 0; i < _agentStartCount; ++i) {
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
			a_accountCash = roll <= 0.75 ? randomDouble(100.0, 1000.0) : randomDouble(1000.0, 40'000.0); // Lower account balances 75% more likely

			if (dispersedShares_R > 1) {
				startingShares = randomUInt(1, dispersedShares_R);
			}
			else {
				startingShares = dispersedShares_R;
			}
			
			dispersedShares_R -= startingShares;
		}
		else {
			a_subType = roll <= algoType_I ? AgentSubType::ALGO : AgentSubType::INFORMED;
			//															    0.001s, 0.1s			    0.2s    1hr
			a_reactionTimeFloor = a_subType == AgentSubType::ALGO ? randomDouble(0.1, 1.0) : randomDouble(200.0, 3'600'000.0);
			a_accountCash = randomDouble(50'000.0, 500'000.0); // TODO: Should be related to OB start price?

			if (dispersedShares_I >= 10) {
				startingShares = randomUInt(unsigned int(dispersedShares_I * 0.10), dispersedShares_I);
			}
			else if (dispersedShares_I > 1) {
				startingShares = randomUInt(1, dispersedShares_I);
			}
			else {
				startingShares = dispersedShares_I;
			}
			
			dispersedShares_I -= startingShares;
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

		if (startingShares > 0) {
			// TODO: Should scale amt with current price?
			//if (startingShares <= 10) {
			roll = randomDouble(0.0, 1.0);
			if (roll <= 0.50) {
				roll = randomDouble(0.0, 1.0);
				if (roll <= 0.50) {
					agent->upsertHolding(Holding(agent->getBetaPrice(this->OB.currentPrice, OrderAction::ASK), startingShares));
				}
				else if (roll <= 0.75) {
					roll = randomDouble(1.0, 10.0);
					agent->upsertHolding(Holding(agent->getBetaPrice(this->OB.currentPrice - (this->OB.currentPrice * roll), OrderAction::ASK), startingShares));
				}
				else {
					roll = randomDouble(1.0, 10.0);
					agent->upsertHolding(Holding(agent->getBetaPrice(this->OB.currentPrice + (this->OB.currentPrice * roll), OrderAction::ASK), startingShares));
				}
			}
			else {
				roll = randomDouble(0.0, 1.0);
				if (roll <= 0.50) {
					agent->upsertHolding(Holding(agent->getBetaPrice(this->OB.currentPrice, OrderAction::BID), startingShares));
				}
				else if (roll <= 0.75) {
					roll = randomDouble(1.0, 10.0);
					agent->upsertHolding(Holding(agent->getBetaPrice(this->OB.currentPrice - (this->OB.currentPrice * roll), OrderAction::BID), startingShares));
				}
				else {
					roll = randomDouble(1.0, 10.0);
					agent->upsertHolding(Holding(agent->getBetaPrice(this->OB.currentPrice + (this->OB.currentPrice * roll), OrderAction::BID), startingShares));
				}
			}
			//}
		}
		
		// Passive quoting agents claim an early slot with their fast floor, then back
		// off to a re-quote cadence. A fill wakes them again, so a stale quote is
		// replaced on being hit rather than after waiting out the timer.
		//													  1s        60s
		if (a_subType == AgentSubType::ALGO) { agent->idleReactionTimeFloor = randomDouble(1000.0, 60'000.0); }

		// Upsert Agent
		this->OB.upsertAgent(agent);

		// All agents here hold shares, initAgents will be for agents that can populate the orderbook, all the float could be dispersed? or just disperse until agent limit hit. All dispersed could cause a hyper active market where every single holder is active, not realistic
		// Non-holding agents will be added seperately?
		// get random share count based on float
		// while share count is above 10
		// random chance 2 of ASK or BID side price
		// random chance 3 of being random percent below or above beta price
		// add holding(s)
	}
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
		std::shared_ptr<Agent> agent = this->OB.agents[nextEventCall.agentId];

		// Discard calls left behind when an agent was woken early
		if (agent == nullptr || nextEventCall.generation != agent->eventGeneration) {
			this->eventCallQueue.pop();
			continue;
		}

		this->eventCallQueue.pop();
		agent->hasPendingEvent = false;
		clock.simTimeMs = nextCallTime;

		// Agent has stepped out of the market, drop it until a sweep brings it back
		if (!agent->isParticipating(this->OB.session, clock.simTimeMs)) { continue; }

		agent->actRandom();
		this->scheduleNextEventCall(agent, clock.simTimeMs);
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

	// Seeds the queue with whoever is actually trading at the premarket open
	for (const auto& kv : this->OB.agents) { kv.second->hasPendingEvent = false; }
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
	for (const auto& kv : this->OB.agents) { kv.second->hasPendingEvent = false; }

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
		bool participating = agent->isParticipating(this->OB.session, simTimeMs);

		// Bankruptcy is a lifecycle state, never overwrite it with a session state
		if (agent->status != AgentStatus::BANKRUPT) {
			agent->status = participating ? AgentStatus::ACTIVE : AgentStatus::INACTIVE;
		}

		// Agents that have just joined need an event, ones that dropped out simply
		// stop being rescheduled and fall out of the queue on their own
		if (participating && !agent->hasPendingEvent) {
			this->scheduleNextEventCall(agent, simTimeMs);
		}
	}

	this->nextParticipationSweepMs = simTimeMs + MarketCalendar::minutesToMs(PARTICIPATION_SWEEP_MINUTES);
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

void CoreSim::setParameters(unsigned int seed, unsigned int backDataDays, Session liveStartSession, unsigned int minLiquidity, unsigned short agentStartCount, unsigned int obShareFloat, double obStartPrice) {

	this->parameters.seed = seed;
	this->parameters.backDataDays = backDataDays;
	this->parameters.liveStartSession = liveStartSession;
	this->parameters.minLiquidity = minLiquidity;
	this->parameters.agentStartCount = agentStartCount;
	this->parameters.obShareFloat = obShareFloat;
	this->parameters.obStartPrice = obStartPrice;

	setSeed(this->parameters.seed);
}