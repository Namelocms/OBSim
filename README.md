# OBSim

_Full depth agent-based market modeling_

![C++](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![CMake](https://img.shields.io/badge/CMake-%3E%3D3.31-blue.svg)
![Status](https://img.shields.io/badge/status-active--development-yellow.svg)

<img width="1904" height="1020" alt="OBSim070726_demo" src="https://github.com/user-attachments/assets/a9a05414-c5c6-45bc-a211-245d72065caa" />

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


- [x] Matching engine: price-time priority, market/limit orders, full escrow system, self-trade protections
- [x] Order Book: `std::set` based bid/ask queues, O(log N) operations, snapshot api
- [x] Agents: NOISE, MOMENTUM, ALGO, INFORMED subtypes with differentiated reaction times and parameters (All semi-random for now)
- [x] Simulation Clock: Pause, step-forward, speed-adjustable, deterministic via `SimClock` timestamps
- [x] Terminal UI: Live candlestick chart, orderbook, agent table, event log, reset dialog using [FTXUI](https://github.com/ArthurSonzogni/FTXUI/tree/main)
- [ ] Robust UI: A more refined UI for better UX and data displays, possibly web-based or using python
- [ ] Agent population composition refinement
- [ ] Extended hours phases
- [ ] Agent lifecycles and exit conditions
- [ ] User order placements in UI
- [ ] API for order placements (for algos/AI)
- [ ] Technical indicators maybe ablility for custom indicators?

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
	    1,		// Initialization Ticks
	    100,		// Agent Start Count
	    250'000,	// Share Float
	    1.00		// Start Price
    );
    ```
2. In the TUI:

    Press `R`, a dialog box will pop up, enter your desired start-up parameters, click enter.
    - Be careful with the price option as it can sometimes cause the simulation to freeze and crash. Stick to just _**two decimal places**_ for now.

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

## Contact
Check out my [LinkedIn](https://www.linkedin.com/in/sean-coleman-974652270) or start a [discussion](https://github.com/Namelocms/OBSim/discussions) in this repo!
