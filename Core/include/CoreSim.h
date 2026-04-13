#include <queue>
#include <string>
#include <memory>
#include <chrono>
#include <vector>
#include <iostream>
#include <thread>

class Agent;
class OrderBook;
class MatchingEngine;
class SimClock;

struct Config {
    unsigned int seed;
    unsigned int initializationTicks;
    unsigned short agentStartCount;
    unsigned int obShareFloat;
    double obStartPrice;
};

struct EventCall {
    double callTime;
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
    bool isRunning;
    bool shouldGetSnapshot;
    std::priority_queue<EventCall, std::vector<EventCall>, CompareEventCalls> eventCallQueue;

    // ---- Main Simulation Loop ----

    void run(SimClock& clock);

    // ---- Simulation Initialization Functions ----

    void initAgents(unsigned short _agentStartCount, OrderBook& OB, MatchingEngine& ME);
    void initMarket(unsigned int _tickCount, OrderBook& OB, SimClock& clock);

    // ---- Event Functions ----

    void scheduleNextEventCall(std::shared_ptr<Agent> agent, double simTimeMs);

    // ---- Utility Functions ----

    void setParameters(unsigned int seed=1, unsigned int initializationTicks=100, unsigned short agentStartCount=100, unsigned int obShareFloat=250'000, double obStartPrice=1.00);

private:
    Config parameters;
};