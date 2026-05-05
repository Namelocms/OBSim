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
	
	this->OB.resetToInitial(this->parameters.obStartPrice, this->parameters.obShareFloat, true);  // = OrderBook(clock, this->parameters.obStartPrice, this->parameters.obShareFloat);
	this->OB.clock = &clock;

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
	double a_reactionTime;
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
			if (a_subType == AgentSubType::NOISE) { a_reactionTime = randomDouble(200.0, 3'600'000.0); }
			//																			  0.5s     15min
			else if (a_subType == AgentSubType::MOMENTUM) { a_reactionTime = randomDouble(5000.0, 900'000.0); }
			//																		  0.01s    2s
			else if (a_subType == AgentSubType::ALGO) { a_reactionTime = randomDouble(10.0, 2000.0); }
			//									 0.2s        1hr
			else { a_reactionTime = randomDouble(200.0, 3'600'000.0); }

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
			a_reactionTime = a_subType == AgentSubType::ALGO ? randomDouble(0.1, 1.0) : randomDouble(200.0, 3'600'000.0);
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
			a_reactionTime,
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
void CoreSim::initMarket(unsigned int _tickCount, SimClock& clock) {
	while (this->OB.tickCount < _tickCount) {
		for (const auto& kv : this->OB.agents) {
			kv.second->actRandom();
			if (this->OB.tickCount >= _tickCount) { break; }
		}
		// Notify TUI of progress during initialization
		if (this->onTick) { this->onTick(); }
	}

	// Change to Regular market hours
	this->OB.session = Session::REGULAR;

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