#include <queue>
#include <string>
#include <memory>
#include <chrono>
#include <vector>

#define NOW std::chrono::high_resolution_clock::now()

class Agent;

struct EventCall {
    long long callTime;
    std::string agentId;
};

/* Closest call time is first in queue */
struct CompareEventCalls {
public:
    bool operator()(const EventCall& e1, const EventCall& e2) {
        return e1.callTime > e2.callTime;
    }
};

class CoreSim {
public:
    std::priority_queue<EventCall, std::vector<EventCall>, CompareEventCalls> eventCallQueue;

    // ---- Event Functions ----

    void scheduleNextEventCall(std::shared_ptr<Agent> agent);

    // ---- Utility Functions ----

    long long getCurrentTimeMS();
};