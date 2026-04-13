#include "include/CoreSim.h"
#include "OrderBook.h"
#include "MatchingEngine.h"
#include "include/Agent.h"
#include "include/Holding.h"
#include "include/Enums.h"
#include "include/Util.h"
#include "include/SimClock.h"

// ---- Main Simulation Loop ----

void CoreSim::run(SimClock& clock) {
	OrderBook OB = OrderBook(this->parameters.obStartPrice, this->parameters.obShareFloat);
	MatchingEngine ME = MatchingEngine(OB);
	
	std::cout << "Initializing Agents..." << std::endl;
	this->initAgents(this->parameters.agentStartCount, OB, ME);

	std::cout << "Initializing Market..." << std::endl;
	this->initMarket(this->parameters.initializationTicks, OB, clock);

	clock.start();
	this->isRunning = true;
	this->shouldGetSnapshot = false;
	unsigned int lastTick = 0;

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

		std::shared_ptr<Agent> agent = OB.agents[nextEventCall.agentId];
		// ================================================================ Check if an eventToken valid?

		// Pace sim with wall clock, sleep if sim is ahead
		double simTarget = clock.simTargetMs();
		if (nextEventCall.callTime > simTarget) {
			double sleepMs = (nextEventCall.callTime - simTarget) / clock.speedMultiplier.load();
			std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(sleepMs));
		}

		// Process Event
		this->eventCallQueue.pop();
		clock.simTimeMs = nextEventCall.callTime;
		agent->actRandom();
		this->scheduleNextEventCall(agent, clock.simTimeMs);

		// Handle step mode
		if (clock.step.load()) {
			clock.step.store(false);
			clock.pause();
		}

		if (OB.getTick(lastTick) >= 12) {
			this->shouldGetSnapshot = true;
		}

		// Capture Snapshot, UI update
		if (this->shouldGetSnapshot) {
			Snapshot snap = OB.getSnapshot();
			std::cout << snap.currentPrice << " @ " << OB.getTick() << " -- " << clock.simTimeMs << std::endl;
			this->shouldGetSnapshot = false;
			lastTick = OB.getTick();
		}
		
	}
}

// ---- Simulation Initialization Functions ----

void CoreSim::initAgents(unsigned short _agentStartCount, OrderBook& OB, MatchingEngine& ME) {
	for (unsigned short i = 0; i < _agentStartCount; ++i) {
		std::shared_ptr<Agent> agent = std::make_shared<Agent>(OB.makeId(ID_TYPE::AGENT), randomDouble(225.0, 25000.0), randomDouble(100.00, 10'000.00), AgentStatus::ACTIVE, OB, ME);
		agent->upsertHolding(Holding(agent->getBetaPrice(OB.currentPrice, OrderAction::ASK), randomInt(1, int(OB.shareFloat * 0.025))));
		agent->upsertHolding(Holding(agent->getBetaPrice(OB.currentPrice, OrderAction::BID), randomInt(1, int(OB.shareFloat * 0.025))));
		OB.upsertAgent(agent);
	}
}
void CoreSim::initMarket(unsigned int _tickCount, OrderBook& OB, SimClock& clock) {
	while (OB.getTick() < _tickCount) {
		for (const auto& kv : OB.agents) {
			kv.second->actRandom();
			if (OB.getTick() >= _tickCount) { break; }
		}
	} 

	// Schedule initial event calls
	for (const auto& kv : OB.agents) {
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