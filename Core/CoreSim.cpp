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
	clock.reset();

	this->OB = OrderBook(this->parameters.obStartPrice, this->parameters.obShareFloat);
	
	//std::cout << "Initializing Agents..." << std::endl;
	this->initAgents(this->parameters.agentStartCount);

	//std::cout << "Initializing Market..." << std::endl;
	this->initMarket(this->parameters.initializationTicks, clock);

	clock.start();
	this->isRunning = true;
	this->shouldGetSnapshot = false;
	long long lastTick = 0;

	while (this->isRunning) {
		
		// Handle Pause
		while (clock.paused.load() && !clock.step.load()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}

		if (this->eventCallQueue.empty()) {
			this->isRunning = false;
			break;
		}

		const EventCall& nextEventCall = this->eventCallQueue.top();

		std::shared_ptr<Agent> agent = this->OB.agents[nextEventCall.agentId];
		// ================================================================ Check if an eventToken valid?

		// Pace sim with wall clock, sleep if sim is ahead
		double simTarget = clock.simTargetMs();
		if (nextEventCall.callTime > simTarget) {
			if (!this->isRunning) { break; }
			double sleepMs = (nextEventCall.callTime - simTarget) / clock.speedMultiplier.load();
			std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(sleepMs));
		}

		// Process Event
		this->eventCallQueue.pop();
		clock.simTimeMs = nextEventCall.callTime;
		agent->actRandom();
		this->scheduleNextEventCall(agent, clock.simTimeMs);
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
		
	}
}

// ---- Simulation Initialization Functions ----

void CoreSim::initAgents(unsigned short _agentStartCount) {
	for (unsigned short i = 0; i < _agentStartCount; ++i) {
		std::shared_ptr<Agent> agent = std::make_shared<Agent>(this->OB.makeId(ID_TYPE::AGENT), randomDouble(225.0, 25000.0), randomDouble(100.00, 10'000.00), AgentStatus::ACTIVE, this->OB, this->ME);
		agent->upsertHolding(Holding(agent->getBetaPrice(this->OB.currentPrice, OrderAction::ASK), randomInt(1, int(this->OB.shareFloat * 0.025))));
		agent->upsertHolding(Holding(agent->getBetaPrice(this->OB.currentPrice, OrderAction::BID), randomInt(1, int(this->OB.shareFloat * 0.025))));
		this->OB.upsertAgent(agent);
	}
}
void CoreSim::initMarket(unsigned int _tickCount, SimClock& clock) {
	while (this->OB.tickCount < _tickCount) {
		for (const auto& kv : this->OB.agents) {
			kv.second->actRandom();
			if (this->OB.tickCount >= _tickCount) { break; }
		}
		// Notify TUI of progress during initialization
		if (this->onTick) { this->onTick(); }
	} 

	// Schedule initial event calls
	for (const auto& kv : this->OB.agents) {
		scheduleNextEventCall(kv.second, clock.simTimeMs);
	}
}

// ---- Event Functions ----

void CoreSim::scheduleNextEventCall(std::shared_ptr<Agent> agent, double simTime) {
	double jitter = randomDouble(0.0, agent->reactionTime);
	double nextEventCallTime = simTime + agent->reactionTime + jitter;
	
	EventCall ec = EventCall(nextEventCallTime, agent->id);
	this->eventCallQueue.push(ec);
}

// ---- Utility Functions ----

void CoreSim::setParameters(unsigned int seed, unsigned int initializationTicks, unsigned short agentStartCount, unsigned int obShareFloat, double obStartPrice) {
	
	this->parameters.seed = seed;
	this->parameters.initializationTicks = initializationTicks;
	this->parameters.agentStartCount = agentStartCount;
	this->parameters.obShareFloat = obShareFloat;
	this->parameters.obStartPrice = obStartPrice;

	setSeed(this->parameters.seed);
}