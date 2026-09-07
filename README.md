# OBSim

_Full depth agent-based market modeling_

![C++](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![CMake](https://img.shields.io/badge/CMake-%3E%3D3.31-blue.svg)
![Status](https://img.shields.io/badge/status-active--development-yellow.svg)

<img width="1904" height="1020" alt="OBSim070726_demo" src="https://github.com/user-attachments/assets/a9a05414-c5c6-45bc-a211-245d72065caa" />

> OBSim is intended for research purposes. Strategies that work in this simulation are not guaranteed to work in real-life trading situations. It is not intended as financial, trading, or investment advice.

---

## Table of Contents
- [Overview](#overview)
  - [What is OBSim?](#what-is-obsim)
  - [Who is this for?](#who-is-this-for)
  - [Why is this different?](#why-is-this-different)
- [Features](#features)
- [Performance](#performance)
- [Architecture](#architecture)
  - [Core Components](#core-components)
- [Getting Started](#getting-started)
  - [Prerequisites](#prerequisites)
  - [Controls](#controls)
- [Usage / Configuration](#usage--configuration)
  - [Changing Simulation Parameters](#changing-simulation-parameters)
- [Project Structure](#project-structure)
  - [Core](#core)
  - [App](#app)
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
- Support for algorithmic or agent trading is rarely included. (_in-progress_)

_Why not use Monte-Carlo Simulations?_
- Monte-Carlo Simulations are great for getting a line chart distribution of where a price might go. What it cannot do however is provide an in-depth look at the real-time order book, allow simulated agents to directly effect the market, or offer anything besides the price line and value.

## Features
- <sup>_[x] = Implemented, [ ] = Planned or In-Progress_</sup>


- [x] Matching engine: price-time priority (FIFO), market/limit orders, marketable limits, full escrow system, self-trade protections
- [x] Order Book: `std::set` based bid/ask queues, O(log N) operations, snapshot api
- [x] Agents: NOISE, MOMENTUM, ALGO, INFORMED subtypes with differentiated reaction times and parameters (All semi-random for now)
- [x] Simulation Clock: Pause, step-forward, speed-adjustable, deterministic via `SimClock` timestamps
- [x] Terminal UI: Live candlestick chart, orderbook, agent table, event log, reset dialog using [FTXUI](https://github.com/ArthurSonzogni/FTXUI/tree/main)
- [x] Extended hours phases: premarket, regular, afterhours and overnight sessions on a real market calendar, with session-driven order rules and expiry
- [x] Back data initialization: headless, time-bounded warm-up that builds real price history and a populated book before the live sim opens
- [x] Transient agents: short-lived participants that arrive, trade for a drawn tenure, and leave again, on a recycled slot pool
- [ ] Robust UI: A more refined UI for better UX and data displays, possibly web-based or using python
- [ ] Agent population composition refinement
- [ ] Agent lifecycles and exit conditions
- [ ] User order placements in UI
- [ ] API for order placements (for algos/AI)
- [ ] Technical indicators maybe ablility for custom indicators?

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
<img width="2220" height="1380" alt="OBSim Architecture" src="https://github.com/user-attachments/assets/e37f3d96-9e12-4a33-a463-2448f08510fc" />

### Core Components
- OrderBook: Stores all simulation data, including agents, orders, etc. See [OrderBook.h](Core/include/OrderBook.h)
- Matching Engine: Handles the matching of every order placed in the simulation. See [MatchingEngine.h](Core/include/MatchingEngine.h)
- Agents: Represent the actors on the order book and hold their individual data, including cash, shares, etc. See [Agent.h](Core/include/Agent.h)
- Orders: A unique instance holding all information related to each order placed by agents, including number of shares, cost, etc. See [Order.h](Core/include/Order.h)

## Getting Started
#### TUI
If you just want to try the project without the code, download the [latest release](https://github.com/Namelocms/OBSim/releases)
#### Prerequisites
- CMake ≥3.31
- C++20 Compiler
- FTXUI is automatically fetched via CMake `FetchContent`
- Clone into Visual Studio and build there
    - x64-Release is recommended for best performance
    - The `MVP_Benchmark_main.cpp` target is deprecated and should not be used

#### Controls
These are listed at the bottom left of the TUI as well.

- `SPACE` = Pause/Resume
- `S` = Take one step forward
- `R` = Open simulation reset box
- `Q` = Quit the simulation/Exit the program
- `←/-` = Slow down time
- `→/+` = Speed up time
- `T` = Change tick aggregation
- `A/D` = Scroll through agent pages
- `ESC` = Cancel an in-progress back data build and return to the reset dialog

## Usage / Configuration
The simulations use seeds to ensure each run with the same seed and **starting parameters** produce the same simulation runs every time.

#### Changing Simulation Parameters
There are two ways to do this, one in code, the other in the TUI.
1. In code:

    Use the `setParameters` function in `CoreSim.h`. This is used in `main.cpp` for simulation startup:
    ```cpp
    CoreSim sim;
    sim.setParameters(
	    1,			// Seed
	    1,			// Back Data (whole days)
	    Session::REGULAR,	// Live Start Session, the live sim opens here
	    0,			// Min Liquidity, 0 disables the check
	    100,		// Agent Start Count
	    250'000,		// Share Float
	    1.00		// Start Price, the price at the START of the back data
    );
    ```
2. In the TUI:

    Press `R`, a dialog box will pop up, enter your desired start-up parameters, click enter.
    - Be careful with the price option as it can sometimes cause the simulation to freeze and crash. Stick to just _**two decimal places**_ for now.
    - `Live Start` is cycled with `←`/`→` rather than typed.
    - The dialog shows the resulting span live, for example `1290 active min (1770 total)`.

#### Parameters
| Parameter | Meaning |
| :--- | :--- |
| Seed | Fixes the run. Same seed **and** same parameters reproduce a run exactly |
| Back Data (days) | Whole prior days simulated headless before the live sim opens. `0` opens immediately |
| Live Start Session | Which session the live simulation opens in, at that session's open |
| Min Liquidity | Minimum resting orders per side required at handoff. `0` disables it |
| Agent Start Count | Number of agents created at startup |
| Share Float | Total shares dispersed among agents |
| Start Price | Price at the **start of the back data**. The live opening price emerges from trading |
| Transient Agents | Whether short-lived agents enter and leave during the run. Each run draws its own share of them. Off skips the path entirely |

Note that back data consumes random draws, so a run is identified by the whole parameter set rather than the seed alone.

## Project Structure
This project is split into two distinct sections: `Core` and `App` in order to keep related functions close together.

#### Core
This is where the simulation lives. All data, orders, agents, times, etc. are kept here.

#### App
This is responsible for the user interface. It is where data from the `Core` is sent to be viewed and interacted with by the user.

## Known Limitations
- Current agent behavior is semi-structured noise (_research in progress_)
- TUI is limited in scope and ability
- Agent composition/distribution needs fine-tuning
- Price selection may need fine-tuning
- There is no built-in way for a user to easily place orders themselves
- Extended hours participation rates and the retail decay curve are hand-tuned starting values, not calibrated against real market data
- Afterhours currently trades slightly faster per minute than premarket, a consequence of the decay starting at full rate while premarket builds from the overnight floor
- Trade volume scales with agent count, so small populations produce a thin market. A few hundred agents or more is recommended for realistic behavior
- `tickHistory` grows unbounded across long back data spans, and the chart re-aggregates the whole history when the timeframe changes
- Transient agent arrival rate, tenure and reaction times are hand-tuned starting values in the same class as the participation rates, not calibrated against real market data
- Transient agents arrive holding no shares, so they must buy before they can sell. At steady state arrivals and departures balance, but the effect is measurable on the price path
- Price drifts strongly upward over long runs, and does so with or without transient agents. Under investigation

## Contact
Check out my [LinkedIn](https://www.linkedin.com/in/sean-coleman-974652270) or start a [discussion](https://github.com/Namelocms/OBSim/discussions) in this repo!
