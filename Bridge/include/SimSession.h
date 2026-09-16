#pragma once
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "CoreSim.h"
#include "SimClock.h"
#include "MarketCalendar.h"
#include "MarketFrame.h"

/* ---- Owning a run, and publishing what it is doing ----
*
* SimSession is the seam between the engine and any frontend that is not in the same
* thread as it. It owns three things the engine deliberately does not:
*
*   * the sim thread's lifecycle -- start, stop, and restart with new parameters
*   * coalescing -- the engine's callbacks fire at the rate the market trades, which is
*     the wrong rate to redraw at and a hopeless rate to send over a socket
*   * batching -- onLog fires on EVERY agent event, tens of thousands per second at speed
*
* It does NOT lock the engine, because the engine has no lock to take. Every frame is
* built on the sim thread itself, from inside a callback, at a moment when nothing is
* mid-mutation. Consumers never touch the engine at all: they copy a finished frame.
*
* The terminal UI does not use this. It reads the engine directly, as it always has.
*/

/* Parameters for a run, the same set CoreSim::setParameters takes */
struct SimParams {
	unsigned int seed = 1;
	unsigned int backDataDays = 1;
	Session liveStartSession = Session::REGULAR;
	unsigned int minLiquidity = 0;
	/* 0 derives the population from the market cap */
	unsigned int agentCount = 100;
	unsigned int shareFloat = 250'000;
	double startPrice = 1.00;
	/* MEDIAN transient share of the resident population, 0 disables the whole path */
	double transientFraction = 0.0;
};

struct SimSessionConfig {
	/* Frames published per second of WALL time, independent of sim speed
	*
	* The engine can fire callbacks orders of magnitude faster than this. Publishing is
	* rate limited against the wall clock rather than sim time, because it is a human's
	* eyes and a socket being fed, and neither cares how fast the simulation is running.
	*
	* This is a ceiling, not a guarantee. Frames are only built when the engine calls back,
	* and in a quiet market the most frequent caller is onIdle, once per PACING_SLICE_MS
	* (25 ms) -- so roughly 40/s is the practical maximum with nothing trading. Measured 34
	* against a requested 60. Asking for more than 40 is harmless but buys nothing.
	*/
	double framesPerSecond = 30.0;
	/* Agent roster publishes per second, far slower than the frame rate
	*
	* Rows change slowly and are the most expensive part of a frame to build -- collecting
	* and sorting the whole population. Frames in between carry agentsIncluded = false,
	* meaning "unchanged", and the client keeps what it has.
	*/
	double agentRowsPerSecond = 4.0;
	/* Most agent rows carried in one frame; the rest are counted in agentsOmitted */
	int agentRowCap = 1000;
	/* Price levels published per side */
	int bookDepthLevels = 20;
	/* Most log lines carried in one frame; the rest are counted in logsDropped
	*
	* A cap, not a buffer size. At 50x with a large population the engine produces far
	* more lines than any frontend can read, so the honest thing is to carry a bounded
	* sample and say how many were left out.
	*/
	int logLinesPerFrame = 200;
};

class SimSession {
public:
	SimSession(CoreSim& sim, SimClock& clock);
	~SimSession();

	SimSession(const SimSession&) = delete;
	SimSession& operator=(const SimSession&) = delete;

	/* Tunables. Read on the sim thread, so set them before start(). */
	SimSessionConfig config;

	// ---- Lifecycle ----

	/* Configure and launch a run on its own thread. Stops any run already in flight. */
	void start(const SimParams& params);
	/* Ask the run to stop and wait for the thread to unwind. Safe when nothing is running. */
	void stop();
	/* stop() then start(), the sequence a reset dialog needs
	*
	* The terminal UI open-codes this in three places (destructor, reset confirm, back-data
	* cancel). It exists once here so a second frontend cannot get it subtly different.
	*/
	void restart(const SimParams& params);

	/* True once the sim thread has returned, whether it finished or was stopped */
	bool finished() const { return this->finished_.load(); }
	/* True between start() and the thread returning */
	bool active() const { return this->threadRunning_.load(); }

	// ---- Reading ----

	/* Copy the most recent frame. False when none has been published yet. */
	bool latest(MarketFrame& out) const;
	/* Sequence number of the most recent frame, 0 when none */
	unsigned long long latestSequence() const;

	// ---- Controls ----
	//
	// Safe to call from any thread: each one either touches an atomic on the clock or a
	// single double the sim thread reads without caching.

	void pause();
	void resume();
	void togglePause();
	/* Advance exactly one event, then re-pause */
	void step();
	void setSpeed(double multiplier);
	/* Shift the market's neutral sentiment, the user facing bull/bear dial */
	void setSentiment(double value);
	void nudgeSentiment(double delta);
	/* Abort an in-progress back-data run */
	void cancelBackData();

private:
	// ---- Engine callbacks, all of these run on the SIM THREAD ----
	void onTick_();
	void onIdle_();
	void onLog_(LogEntry entry);
	void onBackDataProgress_(BackDataProgress progress);

	/* Publish a frame if enough wall time has passed since the last one */
	void maybePublish_();
	/* Build a frame from current engine state and hand it to consumers */
	void publish_();

	/* Fill the book depth of a frame by aggregating orders into price levels */
	void fillBook_(MarketFrame& frame) const;
	/* Fill the agent roster of a frame, residents first then transients */
	void fillAgents_(MarketFrame& frame) const;
	/* Move trades executed since the last frame into it, flagging any that were lost */
	void fillTrades_(MarketFrame& frame);

	CoreSim& sim_;
	SimClock& clock_;

	std::thread thread_;
	std::atomic<bool> threadRunning_{ false };
	std::atomic<bool> finished_{ false };

	/* The most recent published frame, and the lock guarding it
	*
	* Mutable so latest() can stay const: the lock is an implementation detail of reading,
	* not a modification of the session.
	*/
	mutable std::mutex frameMtx_;
	MarketFrame latestFrame_;
	bool hasFrame_ = false;
	unsigned long long sequence_ = 0;

	/* Log lines accumulated since the last frame, and what did not fit
	*
	* Guarded separately from the frame: onLog fires far more often than a frame is built,
	* and it should never contend with a consumer copying one.
	*/
	std::mutex logMtx_;
	std::vector<FrameLogLine> pendingLogs_;
	long long pendingLogsDropped_ = 0;

	/* Back-data progress, last reported. Guarded by frameMtx_. */
	BackDataProgress backData_;

	/* tickCount as of the last frame, the cursor into the trade history */
	long long lastTradeCount_ = 0;

	std::chrono::steady_clock::time_point lastPublish_;
	std::chrono::steady_clock::time_point lastAgentPublish_;
	bool everPublished_ = false;
};
