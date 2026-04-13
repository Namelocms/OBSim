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
 
    void setSpeed(double multiplier) {
        // Recalibrate wall start so speed change doesn't cause a catch-up burst
        wallStart = std::chrono::steady_clock::now() - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double, std::milli>(simTimeMs / speedMultiplier.load())
            );
        speedMultiplier.store(multiplier);
    }
 
    void pause() {
        paused.store(true);
    }
 
    void resume() {
        // Recalibrate so paused wall time doesn't count
        wallStart = std::chrono::steady_clock::now() - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double, std::milli>(simTimeMs / speedMultiplier.load())
            );
        paused.store(false);
    }
 
    void stepOne() {
        step.store(true);
        paused.store(false);
    }
};