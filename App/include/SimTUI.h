#pragma once
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ftxui/component/component.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"

class CoreSim;
class SimClock;
struct Snapshot;
struct LogEntry;

// ---- Candlestick / Price History ----

struct Candle {
    double open = 0.0;
    double close = 0.0;
    double high = 0.0;
    double low = 0.0;
    int    ticks = 0;   // how many sim ticks in this candle
};

// ---- Reset Dialog Parameters ----

struct ResetParams {
    std::string seed = "1";
    std::string initTicks = "100";
    std::string agentCount = "100";
    std::string shareFloat = "250000";
    std::string startPrice = "1.00";
};

// ---- TUI State (thread-safe snapshot of sim data) ----

struct TUIState {
    std::mutex              mtx;

    // Price history (raw tick log, collapsed into candles by SimTUI)
    std::deque<double>      priceHistory;      // raw tick prices, newest last
    std::deque<Candle>      candles;           // aggregated candles

    // Order book depth
    std::vector<std::pair<double, int>> bids;   // {price, volume}
    std::vector<std::pair<double, int>> asks;   // {price, volume}

    // Agent table rows  {id, cash, holdings, #bids, #asks, status}
    struct AgentRow {
        std::string id;
        double      cash = 0.0;
        int         holdings = 0;
        int         numBids = 0;
        int         numAsks = 0;
        std::string status;
    };
    std::vector<AgentRow>   agents;

    // Sim metrics
    double   currentPrice = 0.0;
    double   spread = 0.0;
    double   simTimeMs = 0.0;
    int      totalTicks = 0;
    int      totalOrders = 0;
    int      totalAgents = 0;
    unsigned shareFloat = 0;

    // Log ring buffer
    std::deque<std::string> logLines;          // newest last, cap 200
};

// ---- Main TUI class ----

class SimTUI {
public:
    SimTUI(CoreSim& sim, SimClock& clock);
    ~SimTUI();

    /* Blocking: launches the FTXUI screen and starts the sim thread. */
    void run();

private:
    // ---- References ----
    CoreSim& sim_;
    SimClock& clock_;

    // ---- Shared State ----
    TUIState    state_;
    std::atomic<bool> simDone_{ false };

    // ---- Sim Thread ----
    std::thread             simThread_;

    // ---- UI Configuration ----
    int         tickframeSize_ = 50;   // ticks per candle
    int         chartTimeframe_ = 0;    // index into timeframeLabels_
    int         agentPage_ = 0;
    int         agentsPerPage_ = 8;

    static constexpr int MAX_CANDLES = 60;
    static constexpr int LOG_CAP = 200;
    static constexpr int OB_DEPTH = 12;

    const std::vector<std::string> timeframeLabels_ = { "10t","50t","100t","250t","500t" };
    const std::vector<int>         timeframeTicks_ = { 10,   50,   100,   250,   500 };

    // ---- Reset Dialog ----
    bool         showResetDialog_ = false;
    ResetParams  resetDraft_;

    // ---- Callbacks installed on CoreSim ----
    void onTick_();
    void onLog_(LogEntry entry);

    // ---- State Refresh (called from onTick_) ----
    void refreshState_();

    // ---- Candle Aggregation ----
    void pushPrice_(double price);

    // ---- Component Builders ----
    ftxui::Element buildHeader_();
    ftxui::Element buildPriceChart_();
    ftxui::Element buildOrderBook_();
    ftxui::Element buildAgentTable_();
    ftxui::Element buildLog_();
    ftxui::Element buildControls_();
    int            resetFocusIdx_ = 0;  // which field is active in reset dialog
    ftxui::Element buildResetDialog_();

    // ---- Helper: render one ASCII candle column ----
    ftxui::Element renderCandleColumn_(const Candle& c, double minP, double maxP, int height);

    // ---- Speed control ----
    void cycleSpeed_(int dir);   // dir = +1 faster, -1 slower
};