#include <atomic>
#include <chrono>

class SimClock {
public:
    std::atomic<double> speedMultiplier { 1.0 };
    std::atomic<bool> paused { false };
    std::atomic<bool> step { false };   // advance one event then re-pause
 
    double simTimeMs = 0.0;
    std::chrono::steady_clock::time_point wallStart;
 
    void start() {
        wallStart = std::chrono::steady_clock::now();
    }
 
    double wallElapsedMs() const {
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - wallStart).count();
    }
 
    // Wall clock target: how far into sim time we should be right now
    double simTargetMs() const {
        return wallElapsedMs() * speedMultiplier.load();
    }
 
    /* Rebase the wall clock so simTargetMs() lines back up with simTimeMs
    *
    * simTargetMs() is measured from wallStart, so ANY jump in sim time must rebase
    * it. Skipping this leaves the pacing logic believing the sim is far ahead of
    * schedule, and it sleeps out the whole gap, which looks like a freeze.
    */
    void rebaseWallClock() {
        wallStart = std::chrono::steady_clock::now() - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double, std::milli>(simTimeMs / speedMultiplier.load())
            );
    }

    void setSpeed(double multiplier) {
        // Store first, then rebase against the NEW speed. Rebasing against the old
        // one leaves wallElapsed scaled wrong and causes the catch-up burst this
        // is meant to avoid.
        speedMultiplier.store(multiplier);
        rebaseWallClock();
    }
 
    void pause() {
        paused.store(true);
    }
 
    void resume() {
        // Recalibrate so paused wall time doesn't count
        rebaseWallClock();
        paused.store(false);
    }
 
    void stepOne() {
        step.store(true);
        paused.store(false);
    }

    void reset() {
        step.store(false);
        paused.store(false);
        simTimeMs = 0.0;
    }
};