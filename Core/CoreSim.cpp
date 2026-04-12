#include "include/CoreSim.h"
#include "include/Agent.h"
#include "include/Util.h"

void CoreSim::scheduleNextEventCall(std::shared_ptr<Agent> agent) {
	long long currentTimeMS = this->getCurrentTimeMS();
	long long nextEventCallTime = currentTimeMS + (agent->reactionTime + randomInt(0, agent->reactionTime));
	
	EventCall ec = EventCall(nextEventCallTime, agent->id);
	this->eventCallQueue.push(ec);
}

long long CoreSim::getCurrentTimeMS() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(NOW.time_since_epoch()).count();
}