#include "include/SimTUI.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

#include "CoreSim.h"
#include "SimClock.h"        // must expose: paused, step, speedMultiplier, simTimeMs
#include "Agent.h"
#include "OrderBook.h"
#include "LogEntry.h"
#include "Enums.h"

#include "ftxui/component/component.hpp"
#include "ftxui/component/component_base.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/color.hpp"

using namespace ftxui;

// ============================================================
//  Helpers
// ============================================================

static std::string fmtDouble(double v, int prec = 2) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(prec) << v;
    return ss.str();
}
static std::string fmtMs(double ms) {
    if (ms < 1000.0)  return fmtDouble(ms, 0) + " ms";
    if (ms < 60000.0) return fmtDouble(ms / 1000.0, 2) + " s";
    return fmtDouble(ms / 60000.0, 2) + " min";
}
static Color priceColor(double open, double close) {
    return (close >= open) ? Color::Green : Color::Red;
}

// ============================================================
//  Constructor / Destructor
// ============================================================

SimTUI::SimTUI(CoreSim& sim, SimClock& clock)
    : sim_(sim), clock_(clock) {}

SimTUI::~SimTUI() {
    sim_.isRunning = false;
    // Clear queue so the sim loop exits immediately rather than processing remaining events
    while (!sim_.eventCallQueue.empty()) sim_.eventCallQueue.pop();
    clock_.resume();
    if (simThread_.joinable())    simThread_.join();
}

// ============================================================
//  Public Entry Point
// ============================================================

void SimTUI::run() {
    auto screen = ScreenInteractive::Fullscreen();

    // ---- Candle timeframe toggle (Tab cycles through options) ----
    // ---- Speed labels ----
    const std::vector<double> speedLevels = { 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 25.0, 50.0 };
    int speedIdx = 2; // default 1.0x

    // ---- Reset dialog field states ----
    std::vector<std::string*> resetFields = {
        &resetDraft_.seed, &resetDraft_.initTicks, &resetDraft_.agentCount,
        &resetDraft_.shareFloat, &resetDraft_.startPrice
    };
    const std::vector<std::string> resetLabels = {
        "Seed", "Init Ticks", "Agents", "Share Float", "Start Price"
    };

    // ---- Install sim callbacks ----
    sim_.onTick = [&]() { this->onTick_(); screen.PostEvent(Event::Custom); };
    sim_.onLog = [&](LogEntry e) { this->onLog_(e); };

    // ---- Start sim thread ----
    simThread_ = std::thread([&]() {
        sim_.run(clock_);
        simDone_.store(true);
        screen.PostEvent(Event::Custom);
        });

    // ---- Component: intercept keyboard ----
    auto renderer = Renderer([&]() -> Element {
        // Build dashboard layout
        if (showResetDialog_) {
            return buildResetDialog_();
        }

        auto header = buildHeader_();
        auto chart = buildPriceChart_();
        auto ob = buildOrderBook_();
        auto agents = buildAgentTable_();
        auto logPanel = buildLog_();
        auto controls = buildControls_();

        // Layout:
        //  [  header                              ]
        //  [ chart (flex)  |  order book (fixed)  ]
        //  [ agents (flex) |  log (flex)           ]
        //  [ controls bar                          ]

        return vbox({
            header,
            separator(),
            hbox({
                chart | flex,
                separator(),
                ob | size(WIDTH, EQUAL, 38),
            }) | flex,
            separator(),
            hbox({
                agents | flex,
                separator(),
                logPanel | flex,
            }) | size(HEIGHT, EQUAL, 12),
            separator(),
            controls,
            }) | border;
        });

    auto eventHandler = CatchEvent(renderer, [&](Event event) -> bool {
        if (showResetDialog_) {
            // Dialog key handling
            if (event == Event::Escape) { showResetDialog_ = false; return true; }
            if (event == Event::Tab || event == Event::ArrowDown) {
                resetFocusIdx_ = (resetFocusIdx_ + 1) % (int)resetFields.size();
                return true;
            }
            if (event == Event::ArrowUp) {
                resetFocusIdx_ = (resetFocusIdx_ - 1 + (int)resetFields.size()) % (int)resetFields.size();
                return true;
            }
            // Backspace
            if (event == Event::Backspace) {
                auto& f = *resetFields[resetFocusIdx_];
                if (!f.empty()) f.pop_back();
                return true;
            }
            // Enter = confirm reset
            if (event == Event::Return) {
                showResetDialog_ = false;

                auto toUInt = [](const std::string& s, unsigned def) -> unsigned {
                    try { return (unsigned)std::stoul(s); }
                    catch (...) { return def; }
                    };
                auto toDbl = [](const std::string& s, double def) -> double {
                    try { return std::stod(s); }
                    catch (...) { return def; }
                    };
                unsigned newSeed = toUInt(resetDraft_.seed, 1);
                unsigned newInitTicks = toUInt(resetDraft_.initTicks, 100);
                unsigned short newAgentCount = (unsigned short)toUInt(resetDraft_.agentCount, 100);
                unsigned newShareFloat = toUInt(resetDraft_.shareFloat, 250000);
                double   newStartPrice = toDbl(resetDraft_.startPrice, 1.0);

                // 1. Tell the sim loop to exit
                sim_.isRunning = false;
                clock_.resume(); // unblock any sleep inside the sim thread

                // 2. Join FIRST — now we're the only thread touching the queue
                if (simThread_.joinable()) simThread_.join();

                // 3. Clear in O(1) via swap rather than popping every element
                std::priority_queue<EventCall, std::vector<EventCall>, CompareEventCalls> empty;
                std::swap(sim_.eventCallQueue, empty);

                sim_.setParameters(newSeed, newInitTicks,
                    newAgentCount, newShareFloat, newStartPrice);
                simDone_.store(false);

                {
                    std::lock_guard<std::mutex> lk(state_.mtx);
                    state_.priceHistory.clear();
                    state_.candles.clear();
                    state_.logLines.clear();
                    state_.bids.clear();
                    state_.asks.clear();
                    state_.agents.clear();
                }

                simThread_ = std::thread([&]() {
                    sim_.run(clock_);
                    simDone_.store(true);
                    screen.PostEvent(Event::Custom);
                    });

                return true;
            }
            // Character input
            if (event.is_character()) {
                *resetFields[resetFocusIdx_] += event.character();
                return true;
            }
            return false;
        }

        // Main keyboard handling
        if (event == Event::Character('q') || event == Event::Character('Q')) {
            sim_.isRunning = false;
            screen.ExitLoopClosure()();
            return true;
        }
        if (event == Event::Character(' ')) {
            if (clock_.paused.load()) clock_.resume();
            else clock_.pause();
            return true;
        }
        if (event == Event::Character('s') || event == Event::Character('S')) {
            // Step
            if (!clock_.paused.load()) clock_.pause();
            clock_.step.store(true);
            return true;
        }
        if (event == Event::Character('+') || event == Event::ArrowRight) {
            if (!clock_.paused.load()) clock_.pause();
            speedIdx = std::min(speedIdx + 1, (int)speedLevels.size() - 1);
            clock_.speedMultiplier.store(speedLevels[speedIdx]);
            clock_.resume();
            return true;
        }
        if (event == Event::Character('-') || event == Event::ArrowLeft) {
            if (!clock_.paused.load()) clock_.pause();
            speedIdx = std::max(speedIdx - 1, 0);
            clock_.speedMultiplier.store(speedLevels[speedIdx]);
            clock_.resume();
            return true;
        }
        if (event == Event::Character('t') || event == Event::Character('T')) {
            chartTimeframe_ = (chartTimeframe_ + 1) % (int)timeframeLabels_.size();
            tickframeSize_ = timeframeTicks_[chartTimeframe_];
            // Re-collapse candles
            std::lock_guard<std::mutex> lk(state_.mtx);
            state_.candles.clear();
            Candle cur;
            int cnt = 0;
            for (double p : state_.priceHistory) {
                if (cnt == 0) cur.open = cur.high = cur.low = cur.close = p;
                cur.close = p;
                cur.high = std::max(cur.high, p);
                cur.low = std::min(cur.low, p);
                ++cnt;
                ++cur.ticks;
                if (cnt >= tickframeSize_) {
                    state_.candles.push_back(cur);
                    cur = {}; cnt = 0;
                }
            }
            if (cnt > 0) state_.candles.push_back(cur);
            while ((int)state_.candles.size() > MAX_CANDLES)
                state_.candles.pop_front();
            return true;
        }
        if (event == Event::Character('r') || event == Event::Character('R')) {
            showResetDialog_ = true;
            resetFocusIdx_ = 0;
            resetDraft_ = ResetParams{};
            return true;
        }
        if (event == Event::Character('a') || event == Event::Character('A')) {
            agentPage_ = std::max(0, agentPage_ - 1);
            return true;
        }
        if (event == Event::Character('d') || event == Event::Character('D')) {
            {
                std::lock_guard<std::mutex> lk(state_.mtx);
                int pages = std::max(1, (int)std::ceil((double)state_.agents.size() / agentsPerPage_));
                agentPage_ = std::min(agentPage_ + 1, pages - 1);
            }
            return true;
        }
        return false;
        });

    screen.Loop(eventHandler);
}

// ============================================================
//  Sim Callbacks
// ============================================================

void SimTUI::onTick_() {
    refreshState_();
}

void SimTUI::onLog_(LogEntry entry) {
    std::lock_guard<std::mutex> lk(state_.mtx);
    std::string line = "[" + fmtMs(entry.simTimeMs) + "] " + entry.text;
    state_.logLines.push_back(std::move(line));
    while ((int)state_.logLines.size() > LOG_CAP)
        state_.logLines.pop_front();
}

// ============================================================
//  State Refresh
// ============================================================

void SimTUI::refreshState_() {
    std::lock_guard<std::mutex> lk(state_.mtx);

    // Price
    double p = sim_.OB.currentPrice;
    state_.currentPrice = p;
    pushPrice_(p);

    // Metrics
    state_.simTimeMs = clock_.simTimeMs;
    state_.totalTicks = sim_.OB.tickCount;
    state_.totalOrders = sim_.OB.bidQueue.size() + sim_.OB.askQueue.size();
    state_.totalAgents = sim_.OB.agents.size();
    state_.shareFloat = sim_.OB.shareFloat;

    // Order book depth
    auto snap = sim_.OB.getSnapshot(OB_DEPTH);
    state_.spread = snap.spread;

    state_.bids.clear();
    for (auto& o : snap.bids)
        state_.bids.emplace_back(o->price, o->volume);

    state_.asks.clear();
    for (auto& o : snap.asks)
        state_.asks.emplace_back(o->price, o->volume);

    // Agent table (cap at 200 for perf)
    state_.agents.clear();
    int cnt = 0;
    for (auto& [id, agent] : sim_.OB.agents) {
        TUIState::AgentRow row;
        row.id = agent->id;
        row.cash = agent->cash;
        row.holdings = agent->getTotalHoldings();
        row.numBids = agent->activeBids.size();
        row.numAsks = agent->activeAsks.size();
        // status string
        switch (agent->status) {
        case AgentStatus::ACTIVE:   row.status = "ACT"; break;
        case AgentStatus::INACTIVE: row.status = "IDL"; break;
        case AgentStatus::BANKRUPT: row.status = "BNK"; break;
        }
        state_.agents.push_back(std::move(row));
        if (++cnt >= 200) break;
    }
}

// ============================================================
//  Candle Aggregation
// ============================================================

void SimTUI::pushPrice_(double price) {
    // state_.mtx already held by caller
    state_.priceHistory.push_back(price);
    while ((int)state_.priceHistory.size() > MAX_CANDLES * tickframeSize_ * 2)
        state_.priceHistory.pop_front();

    // Update current in-progress candle
    if (state_.candles.empty()) {
        Candle c; c.open = c.close = c.high = c.low = price; c.ticks = 1;
        state_.candles.push_back(c);
        return;
    }
    Candle& cur = state_.candles.back();
    cur.close = price;
    cur.high = std::max(cur.high, price);
    cur.low = std::min(cur.low, price);
    ++cur.ticks;

    if (cur.ticks >= tickframeSize_) {
        // Seal this candle, start a new one
        Candle next; next.open = next.close = next.high = next.low = price; next.ticks = 0;
        state_.candles.push_back(next);
        if ((int)state_.candles.size() > MAX_CANDLES)
            state_.candles.pop_front();
    }
}

// ============================================================
//  Header
// ============================================================

Element SimTUI::buildHeader_() {
    std::lock_guard<std::mutex> lk(state_.mtx);

    bool paused = clock_.paused.load();
    double speed = clock_.speedMultiplier.load();

    std::string statusStr = paused ? "||  PAUSED" : ">  RUNNING";
    Color statusCol = paused ? Color::Yellow : Color::Green;
    if (simDone_.load()) { statusStr = "■  DONE"; statusCol = Color::GrayDark; }

    return hbox({
        text(" MARKET SIM") | bold | color(Color::Cyan),
        text("  │  ") | color(Color::GrayDark),
        text("Price: ") | color(Color::GrayDark),
        text("$" + fmtDouble(state_.currentPrice, 4)) | bold | color(Color::White),
        text("  Spread: ") | color(Color::GrayDark),
        text("$" + fmtDouble(state_.spread, 4)) | color(Color::Yellow),
        text("  Ticks: ") | color(Color::GrayDark),
        text(std::to_string(state_.totalTicks)) | color(Color::White),
        text("  Orders: ") | color(Color::GrayDark),
        text(std::to_string(state_.totalOrders)) | color(Color::White),
        text("  Agents: ") | color(Color::GrayDark),
        text(std::to_string(state_.totalAgents)) | color(Color::White),
        text("  Float: ") | color(Color::GrayDark),
        text(std::to_string(state_.shareFloat)) | color(Color::White),
        text("  SimTime: ") | color(Color::GrayDark),
        text(fmtMs(state_.simTimeMs)) | color(Color::White),
        filler(),
        text(statusStr) | bold | color(statusCol),
        text("  Speed: ") | color(Color::GrayDark),
        text(fmtDouble(speed, 2) + "x") | bold | color(Color::Magenta),
        text(" "),
        });
}

// ============================================================
//  Price Chart (candlestick)
// ============================================================

Element SimTUI::renderCandleColumn_(const Candle& c, double minP, double maxP, int height) {
    if (maxP <= minP) {
        return vbox({ text("|") | color(Color::GrayDark) });
    }

    double range = maxP - minP;
    auto toRow = [&](double price) -> int {
        int r = (int)std::round((1.0 - (price - minP) / range) * (height - 1));
        return std::clamp(r, 0, height - 1);
        };

    int highRow = toRow(c.high);
    int openRow = toRow(std::max(c.open, c.close));
    int closeRow = toRow(std::min(c.open, c.close));
    int lowRow = toRow(c.low);

    Color col = (c.close >= c.open) ? Color::Green : Color::Red;

    std::vector<Element> rows;
    for (int row = 0; row < height; ++row) {
        std::string ch = " ";
        if (row == highRow && highRow < openRow)       ch = "│ ";   // upper wick
        else if (row > highRow && row < openRow)       ch = "│ ";   // upper wick body
        else if (row >= openRow && row <= closeRow)    ch = "█ ";   // body
        else if (row > closeRow && row < lowRow)       ch = "│ ";   // lower wick
        else if (row == lowRow && lowRow > closeRow)  ch = "│ ";   // lower wick tip
        rows.push_back(text(ch) | color(col));
    }
    return vbox(std::move(rows));
}

Element SimTUI::buildPriceChart_() {
    std::lock_guard<std::mutex> lk(state_.mtx);

    const int CHART_HEIGHT = 32;

    if (state_.candles.empty()) {
        return window(
            text(" Price Chart [" + timeframeLabels_[chartTimeframe_] + "] "),
            text("Waiting for data...") | center | flex
        ) | flex;
    }

    // Find min/max across visible candles
    double minP = std::numeric_limits<double>::max();
    double maxP = std::numeric_limits<double>::lowest();
    for (auto& c : state_.candles) {
        minP = std::min(minP, c.low);
        maxP = std::max(maxP, c.high);
    }
    // Add 5% padding
    double pad = (maxP - minP) * 0.05 + 1e-9;
    minP -= pad; maxP += pad;

    // Y-axis labels (5 labels)
    std::vector<Element> yAxis;
    for (int i = 0; i < CHART_HEIGHT; ++i) {
        double price = maxP - (maxP - minP) * i / (CHART_HEIGHT - 1);
        if (i % (CHART_HEIGHT / 4) == 0) {
            yAxis.push_back(text(fmtDouble(price, 4)) | color(Color::GrayDark));
        }
        else {
            yAxis.push_back(text("      ") | color(Color::GrayDark));
        }
    }

    // Candle columns
    std::vector<Element> cols;
    cols.push_back(vbox(std::move(yAxis)));
    cols.push_back(text("│") | color(Color::GrayDark) | size(HEIGHT, EQUAL, CHART_HEIGHT));

    for (auto& c : state_.candles) {
        cols.push_back(renderCandleColumn_(c, minP, maxP, CHART_HEIGHT));
    }
    cols.push_back(filler());

    std::string title = " Price Chart [" + timeframeLabels_[chartTimeframe_] + " ticks/candle | T: cycle] ";
    return window(text(title) | color(Color::Cyan),
        hbox(std::move(cols)) | flex
    ) | flex;
}

// ============================================================
//  Order Book
// ============================================================

Element SimTUI::buildOrderBook_() {
    std::lock_guard<std::mutex> lk(state_.mtx);

    // Compute max volume for bar scaling
    int maxVol = 1;
    for (auto& [p, v] : state_.asks) maxVol = std::max(maxVol, v);
    for (auto& [p, v] : state_.bids) maxVol = std::max(maxVol, v);

    auto makeRow = [&](double price, int vol, bool isBid) -> Element {
        int barW = std::max(1, (int)(6.0 * vol / maxVol));
        std::string bar = "";//std::string bar(barW, '');
        for (int b = 0; b < barW; ++b) { bar += "▒"; }
        std::string volStr = std::to_string(vol);
        return hbox({
            text(fmtDouble(price, 4)) | color(isBid ? Color::Green : Color::Red) | size(WIDTH, EQUAL, 10),
            text(" "),
            text(volStr) | color(Color::GrayLight) | size(WIDTH, EQUAL, 7),
            text(bar) | color(isBid ? Color::Green : Color::Red),
            });
        };

    std::vector<Element> askRows;
    // Asks displayed in reverse (best ask at bottom, near spread)
    auto asksCopy = state_.asks;
    std::reverse(asksCopy.begin(), asksCopy.end());
    for (auto& [p, v] : asksCopy)
        askRows.push_back(makeRow(p, v, false));

    std::vector<Element> bidRows;
    for (auto& [p, v] : state_.bids)
        bidRows.push_back(makeRow(p, v, true));

    Element spreadEl = hbox({
        text("  SPREAD ") | color(Color::Yellow) | bold,
        text("$" + fmtDouble(state_.spread, 4)) | color(Color::Yellow),
        });

    std::vector<Element> allRows;
    allRows.push_back(hbox({
        text("PRICE     ") | bold | color(Color::GrayDark),
        text("  VOL  ") | bold | color(Color::GrayDark),
        text("BAR") | bold | color(Color::GrayDark),
        }));
    allRows.push_back(separator());
    allRows.push_back(text(" ── ASKS ──") | color(Color::Red) | bold);
    for (auto& r : askRows) allRows.push_back(r);
    allRows.push_back(separator());
    allRows.push_back(spreadEl);
    allRows.push_back(separator());
    allRows.push_back(text(" ── BIDS ──") | color(Color::Green) | bold);
    for (auto& r : bidRows) allRows.push_back(r);
    allRows.push_back(filler());

    return window(text(" Order Book ") | color(Color::Cyan),
        vbox(std::move(allRows))
    );
}

// ============================================================
//  Agent Table
// ============================================================

Element SimTUI::buildAgentTable_() {
    std::lock_guard<std::mutex> lk(state_.mtx);

    int total = (int)state_.agents.size();
    int pages = std::max(1, (int)std::ceil((double)total / agentsPerPage_));
    agentPage_ = std::clamp(agentPage_, 0, pages - 1);

    int start = agentPage_ * agentsPerPage_;
    int end = std::min(start + agentsPerPage_, total);

    auto col = [](const std::string& s, int w, Color c = Color::White) -> Element {
        return text(s) | size(WIDTH, EQUAL, w) | color(c);
        };

    std::vector<Element> rows;
    rows.push_back(hbox({
        col("ID",      16, Color::GrayDark),
        col("CASH",    10, Color::GrayDark),
        col("HELD",     6, Color::GrayDark),
        col("BIDS",     6, Color::GrayDark),
        col("ASKS",     6, Color::GrayDark),
        col("ST",       4, Color::GrayDark),
        }) | bold);
    rows.push_back(separator());

    for (int i = start; i < end; ++i) {
        auto& a = state_.agents[i];
        Color stCol = Color::White;
        if (a.status == "ACT") stCol = Color::Green;
        if (a.status == "BNK") stCol = Color::Red;
        if (a.status == "IDL") stCol = Color::GrayDark;

        rows.push_back(hbox({
            col(a.id,                        16),
            col("$" + fmtDouble(a.cash, 2),    10, Color::Cyan),
            col(std::to_string(a.holdings),   6, Color::Yellow),
            col(std::to_string(a.numBids),    6, Color::Green),
            col(std::to_string(a.numAsks),    6, Color::Red),
            col(a.status,                     4, stCol),
            }));
    }

    std::string title = " Agents [" + std::to_string(agentPage_ + 1) + "/" + std::to_string(pages)
        + " | A/D: page] ";
    return window(text(title) | color(Color::Cyan),
        vbox(std::move(rows)) | flex
    ) | flex;
}

// ============================================================
//  Log Panel
// ============================================================

Element SimTUI::buildLog_() {
    std::lock_guard<std::mutex> lk(state_.mtx);

    // Show last 8 lines
    std::vector<Element> lines;
    int show = std::min(8, (int)state_.logLines.size());
    for (int i = (int)state_.logLines.size() - show; i < (int)state_.logLines.size(); ++i) {
        lines.push_back(text(state_.logLines[i]) | color(Color::GrayLight));
    }
    if (lines.empty()) lines.push_back(text("No events yet.") | color(Color::GrayDark));

    return window(text(" Event Log ") | color(Color::Cyan),
        vbox(std::move(lines)) | flex
    ) | flex;
}

// ============================================================
//  Controls Bar
// ============================================================

Element SimTUI::buildControls_() {
    bool paused = clock_.paused.load();

    auto key = [](const std::string& k) -> Element {
        return text("[" + k + "]") | bold | color(Color::Yellow);
        };
    auto lbl = [](const std::string& l) -> Element {
        return text(" " + l + "  ") | color(Color::GrayLight);
        };

    return hbox({
        key("Space"), lbl(paused ? "Resume" : "Pause"),
        key("S"),     lbl("Step"),
        key("+/-"),   lbl("Speed"),
        key("T"),     lbl("Timeframe"),
        key("A/D"),   lbl("Agents page"),
        key("R"),     lbl("Reset"),
        key("Q"),     lbl("Quit"),
        });
}

// ============================================================
//  Reset Dialog
// ============================================================

Element SimTUI::buildResetDialog_() {
    const std::vector<std::string> labels = {
        "Seed", "Init Ticks", "Agent Count", "Share Float", "Start Price"
    };
    std::vector<std::string*> fields = {
        &resetDraft_.seed, &resetDraft_.initTicks, &resetDraft_.agentCount,
        &resetDraft_.shareFloat, &resetDraft_.startPrice
    };

    std::vector<Element> rows;
    rows.push_back(text("  RESET SIMULATION  ") | bold | color(Color::Cyan) | center);
    rows.push_back(separator());

    for (int i = 0; i < (int)labels.size(); ++i) {
        bool focused = (i == resetFocusIdx_);
        std::string display = *fields[i] + (focused ? "▌" : " ");

        rows.push_back(hbox({
            text(labels[i] + ": ")
                | color(focused ? Color::White : Color::GrayLight)
                | size(WIDTH, EQUAL, 14),
            text(display)
                | color(focused ? Color::Cyan : Color::GrayDark)
                | bold,
            }) | (focused ? inverted : nothing));
    }

    rows.push_back(separator());
    rows.push_back(hbox({
        text("[Tab/↑↓]") | color(Color::Yellow) | bold, text(" Navigate  "),
        text("[Enter]") | color(Color::Green) | bold, text(" Confirm  "),
        text("[Esc]") | color(Color::Red) | bold, text(" Cancel"),
        }) | center);

    return vbox({
        filler(),
        hbox({
            filler(),
            window(text(" ⚙  Reset Parameters "),
                vbox(std::move(rows)) | size(WIDTH, EQUAL, 44)
            ),
            filler(),
        }),
        filler(),
        });
}