#include <iostream>
#include <chrono>
#include <thread>

#include "OrderBook.h"
#include "MatchingEngine.h"
#include "Agent.h"
//#include "Holding.h"
#include "Enums.h"
//#include "Util.h"

#include "CoreSim.h"

//#define NOW std::chrono::high_resolution_clock::now()

int main() {
	CoreSim CS;
	OrderBook OB;
	MatchingEngine ME = MatchingEngine(OB);
	EventCall EMPTY_EVENT_CALL = EventCall(-1, "none");
	EventCall nextEventCall = EMPTY_EVENT_CALL;

	bool isRunning = true;

	std::shared_ptr<Agent> A1 = std::make_shared<Agent>("A", 100, 100, AgentStatus::ACTIVE, OB, ME);
	std::shared_ptr<Agent> A2 = std::make_shared<Agent>("B", 200, 100, AgentStatus::ACTIVE, OB, ME);

	CS.scheduleNextEventCall(A1);
	CS.scheduleNextEventCall(A2);
	CS.scheduleNextEventCall(A1);
	CS.scheduleNextEventCall(A2);

	while (isRunning) {
		if (!CS.eventCallQueue.empty() || nextEventCall.callTime != EMPTY_EVENT_CALL.callTime) {
			if (nextEventCall.callTime == EMPTY_EVENT_CALL.callTime) {
				std::cout << CS.eventCallQueue.top().callTime << CS.eventCallQueue.top().agentId << std::endl;
				nextEventCall = EventCall(CS.eventCallQueue.top().callTime, CS.eventCallQueue.top().agentId);
				CS.eventCallQueue.pop();
			}
			if (CS.getCurrentTimeMS() >= nextEventCall.callTime) {
				std::cout << CS.getCurrentTimeMS() << std::endl;
				std::cout << "Executing Event Call!" << nextEventCall.agentId << std::endl;
				// Do agent action
				//reset nextEventCall
				nextEventCall = EMPTY_EVENT_CALL;
			}
		}
		else {
			isRunning = false;
		}
	}

	std::cout << "OK" << std::endl;
}