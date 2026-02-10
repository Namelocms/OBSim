# OBSim
*Created for high-frequency trading simulations and agent-based market modeling*

A high-performance C++ matching engine designed for financial simulations. The system currently supports random agent behaviour, limit/market orders, and price-time priority matching with an efficient O(log N) architecture.

## Performance Overview
*   **Throughput:** ~1.3 Million actions per second.
*   **Scaling:** Linear O(N) scaling from 100 to 1,000+ agents.
*   **Engine:** Optimized using `std::set` for price-time priority, `std::unordered_map` for O(1) order lookups, and `std::share_ptr` for in-place data manipulation.

## Core Architecture

### Matching Engine
The engine utilizes a non-destructive iterator pattern to match crossing orders:
*   **Price-Time Priority:** Orders are matched starting with the best price, then the oldest timestamp.
*   **Self-Trade Prevention (STP):** Realistic exchange logic that cancels incoming orders if they would trade against the same agent.
*   **Full Escrow System:** Cash is reserved upon order placement to ensure simulation solvency.

### Order Book (LOB) Structures
*   **Bid Queue:** `std::set` using a "Greater Than" comparator for highest-price priority.
*   **Ask Queue:** `std::set` using a "Less Than" comparator for lowest-price priority.
*   **Order Lookup:** An `unordered_map` linking Order IDs to pointers for immediate O(log N) cancellation.

## Technical Specifications
*   **Language:** C++ 14 / C++ 17 / C++ 20 Compatible.
*   **Standard Library:** Heavy use of `<chrono>` for timing (for now), `<memory>` for smart pointers, and `<set>` for sorted order management.
## Building OBSim

This project uses **CMake** to generate builds for multiple platforms.  
It is recommended to create an **out-of-source build** to keep build files separate from source.

- Use **Release** for better performance
- Use **Debug** for testing or development
- Headers and source files are all in OBSim/Core and OBSim/App
- The simulation logic is in a library (OBSim::Core) and the console UI links to it.

### Prerequisites

- CMake >= 3.20
- A C++ compiler (Visual Studio 2022 or later on Windows, or GCC/Clang on Linux/macOS)

### Recommended Build (Windows / Visual Studio)

1. Open a terminal (PowerShell / Command Prompt)
2. Create and enter a build directory:

```bash
cd D:\OBSimProject\OBSim
mkdir build
cd build
```
##### Generate Visual Studio Project Files
```bash
cmake -G "Visual Studio 17 2022" ..
```
##### Build the Project (choose configuration):
```bash
cmake --build . --config Release
```
##### Executable Location
```bash
build/Release/ConsoleApp.exe
```

### Recommended Build (Linux / macOS)
##### Create and enter a build directory:
```bash
mkdir build
cd build
```
##### Run CMake
```bash
cmake -DCMAKE_BUILD_TYPE=Release ..
```
##### Build
```bash
cmake --build .
```
##### Executable Location
```bash
build/ConsoleApp
```

## Usage Example
```cpp
#include "OrderBook.h"
#include "MatchingEngine.h"
#include "Agent.h"
#include "Enums.h"

const int STEPS = 390;          // Steps per simulation
const int NUM_AGENTS = 100;     // Number of agents in the simulation
const double START_PRICE = 1.00; // Orderbook start price
double START_CASH = 100.00       // Amount of money the agents start with

OrderBook OB = OrderBook(START_PRICE);
MatchingEngine ME = MatchingEngine(OB);

// Initialize Agents and add them to the OrderBook
// Edit this to set up environment as needed
for (int i = 0; i < NUM_AGENTS; ++i) {
  std::shared_ptr<Agent> agent = std::make_shared<Agent>(
    OB.makeId(ID_TYPE::AGENT),
    START_CASH,
    AgentStatus::ACTIVE,
    OB,
    ME
  );

// What shares the agents starts with, price is above current price with ASK and below with BID
  agent->upsertHolding(Holding(agent->getBetaPrice(ob.currentPrice, OrderAction::ASK), 10));
  agent->upsertHolding(Holding(agent->getBetaPrice(ob.currentPrice, OrderAction::BID), 10));

  OB.upsertAgent(agent);
}

// This is where the simulation runs
for (int x = 0; x < STEPS; ++x) {
  for (const auto& kv : OB.agents) {
    kv.second->actRandom();
  }
}

```
## Benchmarks
For detailed performance benchmarks, look at the `Benchmarks.md` file
