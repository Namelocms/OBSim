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

#include "CoreSim.h"

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
    std::string backDataDays = "1";
    /* Index into the selectable live start sessions, cycled rather than typed */
    int         liveStartSessionIdx = 1;   // 0 PREMARKET, 1 REGULAR, 2 AFTERHOURS
    std::string minLiquidity = "0";
    std::string agentCount = "100";
    std::string shareFloat = "250000";
    std::string startPrice = "1.00";
    /* Whether transient agents take part, toggled rather than typed */
    bool        transientAgents = true;
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
        double      sentiment = 0.0;
        std::string status;
        bool        isTransient = false;
    };
    std::vector<AgentRow>   agents;

    // Sim metrics
    double   currentPrice = 0.0;
    double   spread = 0.0;
    double   simTimeMs = 0.0;
    double   marketBaseSentiment = 0.0;
    int      totalTicks = 0;
    int      totalOrders = 0;
    /* Agents actually in the market. NOT OB.agents.size(), which also counts pooled
    *  transient slots -- inert agent objects waiting to be rerolled. */
    int      totalAgents = 0;
    int      residentAgents = 0;
    int      liveTransients = 0;
    /* This run's own transient fraction, drawn at startup. 0 when the feature is off. */
    double   transientFraction = 0.0;
    unsigned shareFloat = 0;
    Session  session = Session::PREMARKET;

    // Back-data progress (valid while the run is in flight)
    BackDataProgress backData;

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
    void onBackDataProgress_(BackDataProgress progress);

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
    ftxui::Element buildBackDataOverlay_();

    // ---- Session display helpers ----
    static std::string sessionLabel_(Session session);
    static ftxui::Color sessionColor_(Session session);

    // ---- Reset dialog field layout ----
    /* Sessions the live sim may open in, in dialog cycle order */
    static constexpr int LIVE_START_SESSION_COUNT = 3;
    /* Dialog rows that are pickers, cycled with ←/→ or space instead of typed.
    *  Both are marked by a nullptr entry in the field vectors. */
    static constexpr int SESSION_FIELD_IDX = 2;
    static constexpr int TRANSIENT_FIELD_IDX = 7;
    /* Most agent rows copied into a snapshot. Residents come first so the default view is
    *  stable; transients follow, since they are the rows that churn. */
    static constexpr int MAX_AGENT_ROWS = 400;
    static Session sessionFromIdx_(int idx);

    // ---- Helper: render one ASCII candle column ----
    ftxui::Element renderCandleColumn_(const Candle& c, double minP, double maxP, int height);

    // ---- Speed control ----
    void cycleSpeed_(int dir);   // dir = +1 faster, -1 slower
};