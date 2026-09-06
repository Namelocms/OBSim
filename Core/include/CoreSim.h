#pragma once
#include <queue>
#include <string>
#include <memory>
#include <chrono>
#include <vector>
#include <iostream>
#include <thread>
#include <functional>
#include <atomic>

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
    /* Target concurrent transient agents as a share of the resident population, 0 disables
    *
    * The whole transient path is skipped when this is 0, consuming no RNG, so a run with
    * transient agents off reproduces a pre-feature run exactly.
    */
    double transientFraction = 0.0;

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

/* Chance that any given overnight session is actually simulated
*
* Kept proportional to how little of the real overnight window is tradeable.
* When the roll fails the clock jumps straight to the next premarket open.
*/
inline constexpr double OVERNIGHT_SIM_PROBABILITY = 0.09;

/* ---- Back-data safety caps ----
*
* Defense in depth. Termination is already guaranteed by simulated time, these
* exist so a future change to agent behavior can never reintroduce a hang.
*/
inline constexpr long long BACK_DATA_MAX_EVENTS = 500'000'000LL;
inline constexpr double BACK_DATA_MAX_WALL_SECONDS = 900.0;
/* Whole extra days the run may add chasing minLiquidity, keeps the handoff on the same session open */
inline constexpr int BACK_DATA_MAX_EXTRA_DAYS = 3;
/* How often the back-data loop checks caps, cancellation, and reports progress */
inline constexpr long long BACK_DATA_CHECK_INTERVAL = 4096;
/* Simulated minutes between participation sweeps
*
* Extended hours participation changes continuously, so dormant agents are
* re-checked on this cadence rather than only at session boundaries.
*/
inline constexpr double PARTICIPATION_SWEEP_MINUTES = 15.0;
/* Longest single wall-clock sleep the live loop will take, in real milliseconds
*
* Pacing waits are sliced to this so a long wait, a thin session or an overnight
* skip can never lock out pause, speed changes or quit.
*/
inline constexpr double PACING_SLICE_MS = 25.0;
/* Quiet-market slices between UI refreshes, so the clock keeps moving with no trades */
inline constexpr int QUIET_TICKS_PER_REFRESH = 8;

/* Snapshot of an in-progress back-data run, pushed to the UI on an interval */
struct BackDataProgress {
    double percent = 0.0;        // 0-100 against the current handoff target
    Session session = Session::PREMARKET;
    int dayIndex = 0;
    double simTimeMs = 0.0;
    double activeMinutes = 0.0;  // tradeable minutes elapsed, overnight excluded
    double totalMinutes = 0.0;   // clock minutes elapsed, overnight included
    double targetMinutes = 0.0;  // total clock minutes to the handoff
    long long events = 0;
    long long ticks = 0;
    int extraDays = 0;           // extra days added chasing minLiquidity
};

struct EventCall {
    double callTime;
    std::string agentId;
    /* Agent event generation this call was created with, stale calls are discarded on pop */
    unsigned long long generation = 0;
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
    /* True while the live event loop is running
    *
    * run() assigns this before the loop starts, but it is read from the UI thread and
    * from tests, so it must be false rather than indeterminate before run() is called.
    */
    bool isRunning = false;
    bool shouldGetSnapshot = false;
    OrderBook OB;
    MatchingEngine ME = MatchingEngine(this->OB);
    std::function<void(LogEntry)> onLog;
    std::function<void()> onTick;
    /* Fired on an interval during the back-data run so the UI can show progress */
    std::function<void(BackDataProgress)> onBackDataProgress;
    /* True while the headless back-data run is in progress */
    std::atomic<bool> backDataRunning{ false };
    std::priority_queue<EventCall, std::vector<EventCall>, CompareEventCalls> eventCallQueue;
    /* Sim time of the next session change, drives boundary processing in both loops */
    double nextBoundaryMs = 0.0;
    /* Set to abort an in-progress back-data run, the TUI raises this from its own thread */
    std::atomic<bool> cancelRequested{ false };
    /* True when the last back-data run was cut short by a safety cap or a cancel */
    bool backDataAborted = false;

    // ---- Main Simulation Loop ----

    void run(SimClock& clock);

    // ---- Simulation Initialization Functions ----

    void initAgents(unsigned short _agentStartCount);
    /* Run the headless back-data simulation, unthrottled, from t = 0 to the live start
    *
    * Bounded by simulated time rather than fill count, so it always terminates even
    * if no trade ever occurs. On return the clock sits exactly on the open of
    * parameters.liveStartSession and every agent has one pending event from there.
    */
    void runBackData(SimClock& clock);

    // ---- Session Functions ----

    /* Process every session boundary falling between the current session and targetSimTimeMs
    *
    * Expires resting orders when an expiring session closes, advances OB.session,
    * and decides whether each overnight window is simulated or skipped. Shared by
    * the back-data run and the live loop.
    * Returns true if the event queue was rebuilt (overnight skip), meaning the
    * caller must re-read the top of the queue rather than reusing it.
    */
    bool processSessionBoundaries(double targetSimTimeMs, SimClock& clock);
    /* Jump the clock to resumeAtMs, drop stale events, and reschedule every agent from there */
    void skipToTime(double resumeAtMs, SimClock& clock);

    // ---- Event Functions ----

    void scheduleNextEventCall(std::shared_ptr<Agent> agent, double simTimeMs);
    /* Re-schedule every agent in OB.wakeQueue to act immediately, invalidating their pending event */
    void drainWakeQueue(double simTimeMs);
    /* Refresh who is taking part right now and schedule anyone who has just joined
    *
    * Agents that drop out are not rescheduled, so they fall out of the queue on
    * their own. Agents that rejoin get a fresh event from simTimeMs.
    */
    void sweepParticipation(double simTimeMs);

    // ---- Transient Agent Functions ----

    /* Give a slot a completely fresh transient personality, as of simTimeMs
    *
    * The single source of truth for what a transient agent is. Both the allocate path and
    * the recycle path go through this, so a reused slot is indistinguishable from a newly
    * created one and the two cannot drift apart.
    *
    * Rerolls everything the constructor drew, so nothing of a previous incarnation
    * survives except the id. Bumps eventGeneration rather than resetting it, leaving any
    * event from the previous incarnation stale.
    *
    * Returns false and changes nothing if the slot is not clean. Holdings and resting
    * orders are a PRECONDITION, never something this clears: an Order carries only its
    * agent's id, so rerolling a slot that still owns orders would hand their fills and
    * escrow to the new occupant.
    */
    bool rollTransientPersonality(std::shared_ptr<Agent> agent, double simTimeMs);
    /* Get a transient agent ready to enter the market at simTimeMs
    *
    * Prefers reviving a pooled slot over allocating a new one, so the agents map reaches a
    * high-water mark instead of growing. Returns nullptr only if the population is capped
    * out or a slot could not be prepared.
    */
    std::shared_ptr<Agent> acquireTransientSlot(double simTimeMs);
    /* Draw and admit this interval's transient arrivals, as of simTimeMs
    *
    * Driven from sweepParticipation rather than from its own timer, so it inherits a
    * cadence that already runs in both loops and at every session boundary. The draw is
    * Poisson over the sim time elapsed since the last check, so an irregular sweep cadence
    * does not distort the rate.
    *
    * Returns immediately, touching no RNG, when transientFraction is 0.
    */
    void processTransientArrivals(double simTimeMs);
    /* Ceiling on live transient agents for the current resident population */
    int transientPopulationCap() const;

    /* Non-transient agents, the denominator the arrival rate scales against */
    int residentCount = 0;
    /* Sim time arrivals were last drawn for, the Poisson interval runs from here */
    double lastArrivalCheckMs = 0.0;

    /* Ids of pooled transient slots waiting to be rerolled
    *
    * An explicit free list rather than a scan of the agents map: deterministic, O(1), and
    * independent of hash ordering. This is the register that makes "modify, never add"
    * possible.
    */
    std::vector<std::string> transientFreeList;
    /* Transient agents currently in the market, pooled slots excluded */
    int liveTransientCount = 0;
    /* Transient slots ever allocated, the high-water mark of the pool */
    int transientSlotsAllocated = 0;
    /* Transient agents that have entered the market over the whole run */
    long long transientArrivals = 0;

    /* Sim time of the next participation sweep */
    double nextParticipationSweepMs = 0.0;

    // ---- Utility Functions ----

    void setParameters(
        unsigned int seed = 1,
        unsigned int backDataDays = 1,
        Session liveStartSession = Session::REGULAR,
        unsigned int minLiquidity = 0,
        unsigned short agentStartCount = 100,
        unsigned int obShareFloat = 250'000,
        double obStartPrice = 1.00,
        double transientFraction = 0.0
    );

private:
    /* Drive the event queue forward to targetMs with no wall clock pacing
    *
    * Returns false when a safety cap tripped or a cancel was requested, true on
    * reaching the target normally.
    */
    bool pumpBackDataEvents(double targetMs, SimClock& clock, long long& eventsProcessed,
        const std::chrono::steady_clock::time_point& wallStart);
    /* Push a progress snapshot to the UI */
    void reportBackDataProgress(const SimClock& clock, long long eventsProcessed);

    /* Total clock time to the handoff, denominator for the progress percent */
    double backDataTargetMs = 0.0;
    /* Extra days added chasing minLiquidity, surfaced in progress */
    int backDataExtraDays = 0;

    Config parameters;
};