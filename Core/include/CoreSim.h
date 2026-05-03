#pragma once
#include <queue>
#include <string>
#include <memory>
#include <chrono>
#include <vector>
#include <iostream>
#include <thread>
#include <functional>

#include "OrderBook.h"
#include "MatchingEngine.h"
#include "LogEntry.h"

class Agent;
class OrderBook;
class MatchingEngine;
class SimClock;

struct Config {
    unsigned int seed = 1;
    unsigned int initializationTicks = 100;
    unsigned short agentStartCount = 100;
    unsigned int obShareFloat = 100'000;
    double obStartPrice = 1.00;
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
    OrderBook OB;
    MatchingEngine ME = MatchingEngine(this->OB);
    std::function<void(LogEntry)> onLog;
    std::function<void()> onTick;
    std::priority_queue<EventCall, std::vector<EventCall>, CompareEventCalls> eventCallQueue;

    // ---- Main Simulation Loop ----

    void run(SimClock& clock);

    // ---- Simulation Initialization Functions ----

    void initAgents(unsigned short _agentStartCount);
    void initMarket(unsigned int _tickCount, SimClock& clock);

    // ---- Event Functions ----

    void scheduleNextEventCall(std::shared_ptr<Agent> agent, double simTimeMs);

    // ---- Utility Functions ----

    void setParameters(unsigned int seed=1, unsigned int initializationTicks=100, unsigned short agentStartCount=100, unsigned int obShareFloat=250'000, double obStartPrice=1.00);

private:
    Config parameters;
};