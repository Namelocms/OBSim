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
#include "MarketCalendar.h"

class Agent;
class OrderBook;
class MatchingEngine;
class SimClock;

struct Config {
    unsigned int seed = 1;
    /* Whole days of back data simulated before the live sim begins */
    unsigned int backDataDays = 1;
    /* The live sim begins at the open of this session */
    Session liveStartSession = Session::REGULAR;
    /* Minimum resting orders per side required at handoff, 0 disables the check */
    unsigned int minLiquidity = 0;
    unsigned short agentStartCount = 100;
    unsigned int obShareFloat = 100'000;
    double obStartPrice = 1.00;

    /* ---- Derived back-data duration ----
    *
    * These two functions are the ONLY place whole days are converted into a
    * duration. To accept arbitrary hour/minute timeframes later, replace these
    * and nothing else needs to change.
    */

    /* Total elapsed sim time of the back-data run, overnight spans included
    *
    * The live sim begins at the open of liveStartSession, backDataDays after t = 0.
    * (days=1, REGULAR) = one full day (1440) + premarket (330) = 1770 minutes
    */
    double backDataDurationMs() const {
        return MarketCalendar::sessionOpenMs(this->liveStartSession, int(this->backDataDays));
    }
    /* Tradeable minutes covered by the back-data run, overnight spans excluded
    *
    * (days=1, REGULAR) = 960 active + 330 premarket = 1290 minutes
    */
    double backDataActiveMinutes() const {
        return (this->backDataDays * MarketCalendar::ACTIVE_MINUTES_PER_DAY)
            + MarketCalendar::msToMinutes(MarketCalendar::sessionOffsetMs(this->liveStartSession));
    }
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

    void setParameters(
        unsigned int seed = 1,
        unsigned int backDataDays = 1,
        Session liveStartSession = Session::REGULAR,
        unsigned int minLiquidity = 0,
        unsigned short agentStartCount = 100,
        unsigned int obShareFloat = 250'000,
        double obStartPrice = 1.00
    );

private:
    Config parameters;
};