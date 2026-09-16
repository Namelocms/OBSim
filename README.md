# OBSim

_Full depth agent-based market modeling_

![C++](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![CMake](https://img.shields.io/badge/CMake-%3E%3D3.31-blue.svg)
![TypeScript](https://img.shields.io/badge/TypeScript-5-blue.svg)
![Status](https://img.shields.io/badge/status-active--development-yellow.svg)

<sub>The terminal UI. A screenshot of the desktop app is still to come.</sub>

<img width="1904" height="1020" alt="OBSim070726_demo" src="https://github.com/user-attachments/assets/a9a05414-c5c6-45bc-a211-245d72065caa" />

> OBSim is intended for research purposes. Strategies that work in this simulation are not guaranteed to work in real-life trading situations. It is not intended as financial, trading, or investment advice.

---

## Table of Contents
- [Overview](#overview)
  - [What is OBSim?](#what-is-obsim)
  - [Who is this for?](#who-is-this-for)
  - [Why is this different?](#why-is-this-different)
- [Features](#features)
- [Interfaces](#interfaces)
  - [Desktop app](#desktop-app)
  - [Browser](#browser)
  - [Terminal UI](#terminal-ui)
  - [The WebSocket API](#the-websocket-api)
- [Performance](#performance)
- [Architecture](#architecture)
  - [Core Components](#core-components)
- [Getting Started](#getting-started)
  - [Prerequisites](#prerequisites)
  - [Building](#building)
  - [Controls](#controls)
- [Usage / Configuration](#usage--configuration)
  - [Changing Simulation Parameters](#changing-simulation-parameters)
- [Project Structure](#project-structure)
- [Known Limitations](#known-limitations)
- [Contact](#contact)

## Overview
### What is OBSim?
OBSim is a day trading focused market microstructure simulator centered around a fully simulated Limit Order Book(LOB) 
aka Order book(OB) and individual agents acting on that central LOB. It provides a nearly infinite set of deterministic 
trading situations, perfect for testing and training algorithms, or yourself.
### Who is this for?
Anyone needing to test or learn in a highly realistic and deterministic environment, including:
- Algo traders
- Quants
- AI Bots
- Me (you)
- New traders trying to learn
- People testing new strategies
- Anyone interested in the stock market (especially day trading)
### Why is this different?
_Why not use paper trading or historical backtesting?_
- Many paper trading or backtesting systems are slow, clunky, and rely soley on current or past market data, with some even restricting 
your trading to only market hours (how dumb!). OBSim takes a different approach by using fully synthetic, deterministic 
simulations allowing you to train whenever, and whatever you want while providing novel trading situations everytime. Without
the reliance on limited historical data you are able to reduce the chances of overfitting any models and it allows AI to 
be tested/trained without the worry of historical data leaking into their decision making.
- Support for algorithmic or agent trading is rarely included. OBSim exposes a documented [WebSocket API](#the-websocket-api) that streams the trade-by-trade feed, book depth and agent state to any client you care to write — though placing orders through it is still to come.

_Why not use Monte-Carlo Simulations?_
- Monte-Carlo Simulations are great for getting a line chart distribution of where a price might go. What it cannot do however is provide an in-depth look at the real-time order book, allow simulated agents to directly effect the market, or offer anything besides the price line and value.

## Features
- <sup>_[x] = Implemented, [ ] = Planned or In-Progress_</sup>


- [x] Matching engine: price-time priority (FIFO), market/limit orders, marketable limits, full escrow system, self-trade protections
- [x] Order Book: `std::set` based bid/ask queues, O(log N) operations, snapshot api
- [x] Agents: NOISE, MOMENTUM, ALGO, INFORMED subtypes with differentiated reaction times and parameters (All semi-random for now)
- [x] Simulation Clock: Pause, step-forward, speed-adjustable, deterministic via `SimClock` timestamps
- [x] Desktop app: candlestick chart with volume, depth ladder, agent roster, event log and reset dialog, in a single self-contained executable
- [x] Browser UI: the same page, served over loopback, so anything with a browser can watch a running simulation
- [x] Terminal UI: Live candlestick chart, orderbook, agent table, event log, reset dialog using [FTXUI](https://github.com/ArthurSonzogni/FTXUI/tree/main)
- [x] WebSocket API: a documented JSON protocol carrying the trade stream, book depth, agent state and sim controls
- [x] Extended hours phases: premarket, regular, afterhours and overnight sessions on a real market calendar, with session-driven order rules and expiry
- [x] Back data initialization: headless, time-bounded warm-up that builds real price history and a populated book before the live sim opens
- [x] Transient agents: short-lived participants that arrive, trade for a drawn tenure, and leave again, on a recycled slot pool
- [ ] Short selling and margin — the missing half of the order model, and a prerequisite for calibrating price behavior (see Known Limitations)
- [ ] Agent population composition refinement
- [ ] Agent lifecycles and exit conditions
- [ ] User order placements in UI
- [ ] Order placement over the API (for algos/AI) — the protocol reserves the message, it is not implemented yet
- [ ] Technical indicators, including custom ones — now a client-side job, since the API streams individual trades rather than finished candles
- [ ] Tick-count bars in the web UI, alongside the time-based ones

### Market Scale
**An agent is a unit of resolution, not a person.** `Share Float × Start Price` says how big the market is; `Agent Start Count` says how finely that market is resolved. Every per-agent quantity — shares *and* cash — is then derived as market size ÷ resolution, never set as an absolute amount.

This matters because it is the difference between a market that behaves and one that does not. If cash endowments are fixed dollar amounts, adding agents adds money to a market whose float never grew, and the price has nowhere to go but up until the two agree again. Denominating everything in market cap removes the contradiction at the source rather than damping its symptom.

- **Share dispersal.** Each agent draws a lognormal weight and the float is apportioned against those weights, so the number of holders follows the population and the float is exhausted exactly at any size. The institutional share of the float is drawn per run rather than fixed, so ownership structure varies from market to market.
- **Cash endowments.** The account-size bands are a *shape*, not an amount. One per-run scale converts them into money for a market of this size, so the population collectively holds about as much cash as the stock is worth. That balance point is where price is stationary — anything else would decide in advance which way price ought to move, which is a judgement that has no business inside the agents.
- **Derived population.** Setting `Agent Start Count` to `0` sizes the population to the market cap instead, at roughly the point where the account bands read as literal dollars. The reset dialog shows the number it will use and an estimated back-data time before you commit, because a large float implies a lot of agents.

Both dials stay independent on purpose. Resolution is yours to choose; market size is a separate question; and sweeping either one is a valid experiment.

> The balance point is a **long-only** result. Cash and stock are the only two things an agent can hold today, which is what makes an even split the neutral one. Once short selling exists a position can be negative, and short proceeds, margin and borrow all move where price is stationary — so this is measured and correct for the market as it currently is, not a constant of nature. See Known Limitations.

### Market Sessions
The simulation runs on a real US equity calendar. `t = 0` is 04:00 on day 0, and a day is 1440 clock minutes of which 960 are tradeable.

| Session | Clock | Length | Notes |
| :--- | :--- | :--- | :--- |
| PREMARKET | 04:00 - 09:30 | 330 min | No market orders. Rolls into REGULAR without expiring orders |
| REGULAR | 09:30 - 16:00 | 390 min | Full participation, market orders allowed |
| AFTERHOURS | 16:00 - 20:00 | 240 min | No market orders. Resting orders expire at the close |
| OVERNIGHT | 20:00 - 04:00 | 480 min | Only simulated occasionally, otherwise the clock jumps to the next premarket open |

Session drives three behaviors:
- **Order types.** Market orders are restricted to regular hours, and are never used by institutions or algos, which cross with marketable limit orders instead.
- **Order expiry.** Resting orders carry an expiry and are swept at the regular, afterhours and overnight closes. Premarket is not an expiring boundary, so a day order placed premarket lives through the regular session.
- **Participation.** Outside regular hours the market has *fewer participants*, not slower ones. Retail participation decays continuously from the afterhours open through the overnight, then builds back through premarket, while institutional desks stay at a flat rate. Extended hours settle at roughly 5% of daily volume.

### Back Data Initialization
Before the live simulation opens, OBSim runs the real engine headless and unthrottled across a configurable span of prior sessions, so the market you are handed already has price history, a populated order book, and agent portfolios that evolved through actual trading.

- Bounded by **simulated time**, never by fill count, so it always terminates even if no trade occurs
- Runs every session in order, including the overnight decision, and hands off exactly at the open of the session you choose
- Optional minimum liquidity target extends the run a whole day at a time so the handoff still lands on the same session open
- Progress overlay with a cancel key, since a long span can take a moment

### Transient Agents
The resident population is fixed for the life of a run, which makes activity a constant function of agent count. Transient agents are the floating part: retail takers who show up, trade for a while, and leave. They contribute roughly a fifth of regular-session volume in a median run, more or less depending on what that run drew, and they are what keeps extended hours from going silent in a small market.

- **Arrival** is a Poisson process evaluated on the existing participation sweep. Its rate is expressed as a target *steady-state concurrency* as a share of the resident population, converted by Little's law, so transient activity stays proportional as agent count changes instead of needing retuning at every size. Each session has its own rate multiplier.
- **Tenure** is a fixed commitment window followed by an exponential hazard with a per-agent half-life, hard-capped. The hazard runs on elapsed *simulated time*, not per action, so an agent that acts every two seconds and one that acts every fifteen minutes draw from the same distribution — a lifespan is never an accident of reaction speed.
- **Departure** is driven by the agent's own averaged sentiment turning against the way it is positioned. It is expressed against `Agent::directionalBias` rather than against the sign of sentiment, so a short-side agent added later inherits the whole mechanism by flipping one number.
- **Unwinding.** A leaving agent cancels its entry-side orders and works its position off, always crossing and always for the whole remaining size. Participation gating is suspended while it does, or an agent that went dormant off hours could never get flat. One that cannot clear inside its grace window is *stranded*, not retired: its cadence stretches to hours and it keeps working the position off, so the float keeps circulating.
- **Per-run variation.** The configured fraction is a *median*, not a fixed setting. Each run draws its own around it, so one seed opens a market thick with short-term traders and another a quiet one, instead of every run of a given size having identical participant composition. Most runs land near the median; a noticeably thick or thin market is occasional.
- **Recycling.** Agents are never destroyed. A departed agent's slot returns to a pool and the next arrival revives it with a completely rerolled personality, so `OB.agents` reaches a high-water mark instead of growing. Slots are reused 10-20 times each over a few days.

Toggle them from the reset dialog. Off is exactly off: the whole path is skipped and consumes no random draws, so a run reproduces one from before the feature existed.

## Interfaces
There are three ways to watch a simulation, and they are not three programs. All of them run the same engine and two of them load the same page over the same socket — the desktop app is the browser client in a window, not a separate product with separate capabilities.

The engine sends **individual trades and periodic state frames**. Candles, timeframes and indicators are built by whoever is drawing, which is why any timeframe is available instantly and why a custom indicator is a client-side question rather than an engine feature.

### Desktop app
`OBSim_App.exe` — the main product. One file, about 2 MB, with the UI compiled into the binary. No install, no folder of assets beside it, no console window.

- Candlestick chart with a volume histogram on a **real time axis**, seeded on open with the price history the back-data run already built
- Depth ladder aggregated **by price level**, showing size and order count per level
- Agent roster with a filter, live cash, holdings, resting orders and sentiment
- Event log, session and clock readouts, speed and sentiment controls, and a reset dialog

> **Needs the WebView2 runtime.** It ships with Microsoft Edge, so Windows 10 and 11 have it already and nothing needs installing. Older machines may not — if the app starts and no window appears, install the [WebView2 Evergreen Runtime](https://developer.microsoft.com/microsoft-edge/webview2/). It is deliberately **not** bundled: doing so would roughly double the download for the large majority of users who already have it.

### Browser
`OBSim_Server.exe` runs the same engine headless and serves the same UI at `http://127.0.0.1:8788`. Useful for watching a long run from another window, or on a machine where you would rather not open an app.

The page takes `?server=host:port` to point it at an engine on a different port.

### Terminal UI
`OBSim_ConsoleApp.exe` — the original FTXUI interface, still supported and unchanged. It talks to the engine directly rather than over the socket, so it has no dependency on any of the above and runs anywhere a terminal does.

### The WebSocket API
The desktop app and the browser UI are both clients of a documented protocol on `ws://127.0.0.1:8787`, and so can anything else be. It is plain JSON so you can read it in a browser's network tab or write a bot against it without a schema.

The server sends `hello` (the epoch base, session lengths and run parameters), `backfill` (the retained trade history, so a client joining mid-run can draw a chart), and then `frame` about 30 times a second: price, spread, session, book depth, agent rows, log lines and every trade since the previous frame. Clients send `pause`, `resume`, `step`, `speed`, `sentiment`, `backfill` and `reset`.

Repeated data is sent as tuples rather than objects — a book level is `[price, volume, orders]`, a trade is `[epochSec, price, volume, side]` — and `hello` states those layouts so a client can check it agrees with the server. Everything binds `127.0.0.1` only and is unreachable from off the machine.

> Order placement is **not** implemented. The protocol recognises an `order` message and refuses it by name, which reserves the shape so adding it later is a new message rather than a breaking change.

## Performance
These are slightly outdated since this was done on the inital MVP, pre-TUI, but performance is comparable, if not better now.
Benchmarks use step-based runtimes which were based on how many minutes would be in the comparable hour-based alotment. For example 
a 6.5hr run would be 390 steps because there are 390 minutes in 6.5 hours. These choices were made before settling on a continuous
runtime model for OBSim and were used in my initial python prototyping so they provided a nice comparison. A more robust benchmark would be beneficial.

<sub>Check out the full initial MVP Benchmarks [here](https://github.com/Namelocms/OBSim/blob/master/Benchmarks.md)</sub>
<details>
<summary><b>System Specifications</b></summary>

*   **CPU:** Intel(R) Core(TM) i7-7700K @ 4.20Hz
*   **RAM:** 16 GB
*   **OS:** Windows 10
*   **Compiler:** Visual Studio (2022)
*   **Build Config:** Release x64

</details>
<details>
<summary><b>Performance Analysis (AI Summary)</b></summary>

#### Correlation Analysis: 100 vs. 1,000 Agents

The benchmark data demonstrates a strong **linear scaling relationship**. As the workload increased (Agent Count) by a factor of **10x**, the execution time increased by an average factor of **11.06x**. 

This indicates that the engine complexity is near-optimal, maintaining high efficiency even as the simulation scale expands.

---

#### 1. Scaling Factor Analysis
The table below shows the "Scaling Multiplier" (how much slower the 1,000-agent run was compared to the 100-agent run):

| Simulation Type | 100 Agents (ms) | 1,000 Agents (ms) | Scaling Multiplier |
| :--- | :--- | :--- | :--- |
| **Market (6.5hr)** | 29.79 ms | 313.54 ms | **10.52x** |
| **Extended (16hr)** | 71.05 ms | 841.78 ms | **11.84x** |
| **24hr Market** | 105.40 ms | 1151.35 ms | **10.92x** |
| **Averages** | **68.75 ms** | **768.89 ms** | **11.06x** |

**Interpretation:** A perfect linear system would scale at exactly **10.0x**. The result of **~11x** is excellent, as the extra 1x is the expected overhead of managing larger `std::set` trees O(log(N)) and increased cache misses in the CPU.

---

#### 2. Throughput Efficiency
Throughput measures "Actions per Second." A stable throughput across different scales indicates a robust architecture.

| Agent Count | Average Throughput | Efficiency Retained |
| :--- | :--- | :--- |
| **100 Agents** | 1,342,063 act/sec | 100% (Baseline) |
| **1,000 Agents** | 1,211,663 act/sec | **90.2%** |

**Conclusion:** Throughput retention was 90.2% at 10x agent scale, suggesting the engine scales sub-linearly with low overhead in this range, though more scale points would be needed to confirm this trend holds at higher loads.

---

#### 3. Key Technical Observations
*   **Avoidance of O(N^2) Traps:** Because the runtime didn't spike to 100x or 1000x when agents increased by 10x, we can confirm the engine successfully avoids nested loops or redundant data copying.
*   **The "Run 1" Anomaly:** In almost every test, **Run 1** is 15-30% slower than subsequent runs. This confirms the **CPU/Instruction Cache warm-up** effect; the engine becomes faster once the code is "hot" in the processor.
*   **Memory Bound:** The slight dip in throughput at 1,000 agents suggests the simulation is moving from the **L3 Cache** into the **System RAM**.

#### Predictability
Based on this correlation, the engine is highly predictable. If we were to scale to **10,000 Agents**, we could expect a 24hr Market simulation to complete in approximately **12.5 seconds** on the i7-7700K hardware.
</details>

## Architecture
<sub>This diagram predates the web frontend and shows the engine and terminal UI only. The layering below is current.</sub>

<img width="2220" height="1380" alt="OBSim Architecture" src="https://github.com/user-attachments/assets/e37f3d96-9e12-4a33-a463-2448f08510fc" />

Dependencies point one way, which is the point: the engine knows nothing about any interface, and the interfaces are stacked on top of it rather than wired into it.

```
Core ─── Bridge ─── Server ─┬─ Desktop   (WebView2 window)
 │                          └─ Web       (TypeScript UI, compiled into the binaries)
 └────────────────────────── App         (FTXUI terminal UI, talks to Core directly)
```

`Core` and `Bridge` pull in no external libraries at all, so the engine and its publisher stay portable — and remain a candidate for a WebAssembly build later. The JSON and WebSocket libraries live only in `Server`, the webview only in `Desktop`, and FTXUI only in `App`.

### Core Components
- OrderBook: Stores all simulation data, including agents, orders, etc. See [OrderBook.h](Core/include/OrderBook.h)
- Matching Engine: Handles the matching of every order placed in the simulation. See [MatchingEngine.h](Core/include/MatchingEngine.h)
- Agents: Represent the actors on the order book and hold their individual data, including cash, shares, etc. See [Agent.h](Core/include/Agent.h)
- Orders: A unique instance holding all information related to each order placed by agents, including number of shares, cost, etc. See [Order.h](Core/include/Order.h)
- SimSession: Owns the simulation thread and turns the engine's callbacks into snapshots a frontend can read from another thread. The engine has no internal locking, so frames are built on the sim thread and handed over as plain values. See [SimSession.h](Bridge/include/SimSession.h)
- MarketFrame: One update, as a plain value with no references back into the engine — everything a frontend needs to draw a moment. See [MarketFrame.h](Bridge/include/MarketFrame.h)
- Protocol: The JSON wire format, and the only place that parses input the project did not produce. See [Protocol.h](Server/include/Protocol.h)

## Getting Started
If you just want to try the project without the code, download the [latest release](https://github.com/Namelocms/OBSim/releases).

#### Prerequisites
- CMake ≥3.31
- A C++20 compiler
- **Node.js and npm**, to build the web UI. Everything else is fetched automatically by CMake `FetchContent` — FTXUI, IXWebSocket, nlohmann/json, webview and the WebView2 SDK
- **The WebView2 runtime**, for the desktop app only. Already present on Windows 10 and 11 via Edge; see [Desktop app](#desktop-app) if yours is older

> No Node? Configure with `-DOBSIM_BUILD_WEB=OFF`. Everything still builds and the engine still runs and serves its socket — the binaries simply carry no UI. The simulator is useful headless, and a bot does not need a chart.

#### Building
Clone and build with CMake, or open the folder in Visual Studio and build there. **x64-Release is strongly recommended** — Debug is roughly an order of magnitude slower and will mislead you about performance.

The web UI is built as part of the normal build and compiled into the binaries, so there is no separate frontend step and nothing to ship beside the executable.

| Target | What it is |
| :--- | :--- |
| `OBSim_App` | The desktop app. Engine, server and window in one file |
| `OBSim_Server` | Headless engine + WebSocket API + the UI at `http://127.0.0.1:8788` |
| `OBSim_ConsoleApp` | The terminal UI |

<sub>`MVP_Benchmark_main.cpp` is deprecated and should not be used.</sub>

To work on the UI itself, run a Vite dev server against a running engine for instant reloads:

```
OBSim_Server.exe --no-ui
cd Web && npm install && npm run dev
```

#### Controls
The same bindings work in the desktop app, the browser and the terminal UI.

- `SPACE` = Pause/Resume
- `S` = Take one step forward
- `R` = Open simulation reset box
- `←/-` = Slow down time
- `→/+` = Speed up time
- `T` = Cycle the chart timeframe
- `,/.` = Shift market sentiment bearish/bullish
- `ESC` = Close the reset dialog

Terminal UI only:
- `Q` = Quit
- `A/D` = Scroll through agent pages
- `ESC` = Cancel an in-progress back data build and return to the reset dialog

The web UI has clickable controls for all of the above along the bottom, a filter box on the agent roster, and a chart you can pan and zoom — it stops auto-scrolling while you look around and resumes when you return to the live edge.

## Usage / Configuration
The simulations use seeds to ensure each run with the same seed and **starting parameters** produce the same simulation runs every time.

#### Changing Simulation Parameters
Three ways: in code, or from the reset dialog in either UI.
1. In code:

    Use the `setParameters` function in `CoreSim.h`. This is used in `main.cpp` for simulation startup:
    ```cpp
    CoreSim sim;
    sim.setParameters(
        1,                 // Seed
        1,                 // Back Data (whole days)
        Session::REGULAR,  // Live Start Session, the live sim opens here
        0,                 // Min Liquidity, 0 disables the check
        100,               // Agent Start Count
        250'000,           // Share Float
        1.00,              // Start Price, the price at the START of the back data
        0.04               // Transient Agents, as a share of residents. 0 disables
    );
    ```
2. In the desktop app or browser:

    Press `R` or click **Reset run**, fill in the dialog, and press **Go**. The fields are prefilled with what is actually running, not with defaults, so you can change one thing and leave the rest alone.
    - `Live Start` is a dropdown; everything else is typed.
    - Be careful with the price option as it can sometimes cause the simulation to freeze and crash. Stick to just _**two decimal places**_ for now.
    - Setting `Agent Count` to `0` tells you the population it will derive from the market cap before you commit.

3. In the terminal UI:

    Press `R`, a dialog box will pop up, enter your desired start-up parameters, click enter.
    - `Live Start` is cycled with `←`/`→` rather than typed.
    - The dialog shows the resulting span live, for example `1290 active min (1770 total)`.

#### Parameters
| Parameter | Meaning |
| :--- | :--- |
| Seed | Fixes the run. Same seed **and** same parameters reproduce a run exactly |
| Back Data (days) | Whole prior days simulated headless before the live sim opens. `0` opens immediately |
| Live Start Session | Which session the live simulation opens in, at that session's open |
| Min Liquidity | Minimum resting orders per side required at handoff. `0` disables it |
| Agent Start Count | How finely the market is resolved, **not** a headcount of real investors. `0` derives it from the market cap — the dialog shows the number and an estimated runtime first |
| Share Float | Total shares dispersed among agents. With Start Price this sets the market's size, and every per-agent cash and share amount is derived from it |
| Start Price | Price at the **start of the back data**. The live opening price emerges from trading |
| Transient Agents | Whether short-lived agents enter and leave during the run. Each run draws its own share of them. Off skips the path entirely |

Note that back data consumes random draws, so a run is identified by the whole parameter set rather than the seed alone.

## Project Structure
Each folder is a build target, and each one depends only on those to its left. The engine has no idea any of the interfaces exist.

| Folder | What it is |
| :--- | :--- |
| `Core/` | The simulation. Agents, orders, the book, the matching engine, the clock and the market calendar. No external dependencies, no interface code |
| `Bridge/` | Turns the engine's callbacks into self-contained snapshots a frontend can read from another thread, at a sane frame rate. Depends only on `Core` |
| `Server/` | The JSON protocol, the WebSocket server, and an HTTP server for the UI. Also builds `OBSim_Server` |
| `Web/` | The browser UI — TypeScript, no framework. Built by npm during the CMake build and compiled into the binaries |
| `Desktop/` | `OBSim_App`: engine, servers and a WebView2 window. The desktop product |
| `App/` | The FTXUI terminal UI. Reads `Core` directly and is independent of everything above |

#### Why the split
`Bridge` exists because the engine has no internal locking and no notion of a frame. Something has to decide *when* a coherent snapshot exists and how often one is worth taking, and that is neither the simulation's job nor the UI's.

Keeping `Core` and `Bridge` free of external dependencies is deliberate: it keeps the engine portable, keeps the terminal UI independent of the web stack, and leaves a WebAssembly build on the table.

## Known Limitations
- Current agent behavior is semi-structured noise (_research in progress_)
- Agent composition/distribution needs fine-tuning
- Price selection may need fine-tuning
- There is no built-in way for a user to place orders themselves, in any interface, and none over the API either
- Extended hours participation rates and the retail decay curve are hand-tuned starting values, not calibrated against real market data
- Afterhours currently trades slightly faster per minute than premarket, a consequence of the decay starting at full rate while premarket builds from the overnight floor
- Trade volume scales with agent count, so small populations produce a thin market. A few hundred agents or more is recommended for realistic behavior
- Trade history is kept in a bounded window (250,000 trades by default). A run long enough to exceed it can no longer backfill a chart all the way to its start, and says so rather than drawing a shortened history as if it were complete
- The terminal UI is frozen at its current feature set. It still works and is still supported, but new interface work goes to the web UI, so the two will diverge over time
- The desktop app needs the WebView2 runtime, which is present on Windows 10 and 11 but not bundled — see [Desktop app](#desktop-app)
- No technical indicators are implemented yet. The data to compute them client-side is now streamed, but nothing draws them
- The web chart offers time-based bars only. The terminal UI's tick-count bars are not there yet
- Transient agent arrival rate, tenure and reaction times are hand-tuned starting values in the same class as the participation rates, not calibrated against real market data
- Transient agents arrive holding no shares, so they must buy before they can sell. At steady state arrivals and departures balance, but the effect is measurable on the price path

### The market is long-only, and price behavior is parked until it isn't
**Every position in the simulation is long.** There is no short selling, no margin, and no borrow. `Agent::directionalBias` exists and is `+1` for every agent; `adversity()` and the transient departure logic are already written against it rather than against the sign of sentiment, so adding a short-side agent is a bias flip rather than a rewrite. But nothing sets it to `-1` yet.

That single fact shapes more of the model's behavior than it first appears, because **an agent can only sell what it already owns**. Sell-side liquidity is therefore capped by the number of holders, and an agent holding nothing is structurally a buyer until it has bought something. Hunting the strong upward price drift came down to exactly this: the float used to reach only ~20 holders regardless of population, so most of the market could only buy, and that one-sided flow *was* the residual drift once the cash mismatch was accounted for.

Several current results are therefore **conditional on long-only** and should be re-derived once shorts exist, not carried forward:

- **The cash-to-market-cap balance point of 1.0.** It is the neutral value because cash and stock are the only two things an agent can hold, so holding as much cash as stock is the balanced split. A short position is negative stock, and short proceeds, margin requirements and borrow costs all move where price is stationary. The value is measured and correct — for a long-only market.
- **Float conservation as currently stated.** Shares are never created today, and every test asserts the float is conserved exactly. Shorting manufactures synthetic supply, so that invariant becomes something like *long shares − short shares = float*, with borrow tracked separately.
- **Holder count as the ceiling on sell-side depth.** Shorting removes the constraint at its root, which changes what the ownership distribution needs to achieve.
- **The endowment sampling error** noted below, whose price effect was measured in a market where cash is the only buying power and holders the only selling power.

**Deliberate decision: no further tuning of pricing, endowments or dispersal until the order model is complete.** Calibrating against half a market risks fitting the parameters to an artifact of the missing half and then having to unpick it. The mechanism bugs found so far have been fixed because they were defensibly wrong on their own terms — a stick-break that made holder count a `log₂` accident, dollar amounts that ignored the market's size — but anything that amounts to choosing how prices *should* behave waits for shorts.

### Endowment sampling error at small populations
The per-run cash scale is computed from the *expected* type mix, not the realised one. Institutional accounts carry roughly 50× retail's, so the binomial wobble in how many institutions a run actually drew leaks into the market's total purchasing power, by about `2 / √N`. Measured spread of total cash against market cap: **0.115–2.235 at 25 agents**, 0.441–1.618 at 100, 0.913–1.111 at 2,000.

The magnitude is set by the agent count, which means a market's total buying power currently depends on how finely it was resolved — a contradiction of the resolution principle above. Its effect on price is real but a minority contributor and only at small populations: it explains 21% of the day-one price variance at 25 agents and 1% at 2,000.

A fix is understood (rescale once more against the realised mix, and draw any wanted variation explicitly so it is independent of agent count) but **deferred with everything else until shorts land**, since it was measured in a long-only market.

## Contact
Check out my [LinkedIn](https://www.linkedin.com/in/sean-coleman-974652270) or start a [discussion](https://github.com/Namelocms/OBSim/discussions) in this repo!
