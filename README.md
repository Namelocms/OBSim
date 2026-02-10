# OBSim
*Created for high-frequency trading simulations and agent-based market modeling*

A high-performance C++ matching engine designed for financial simulations. The system supports multiple agent behaviors, limit/market orders, and price-time priority matching with an efficient O(log N) architecture.

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
*   **Language:** C++11 / C++14 / C++17 Compatible.
*   **Standard Library:** Heavy use of `<chrono>` for timing (for now), `<memory>` for smart pointers, and `<set>` for sorted order management.
*   **Recommended Build:** Release x64 with Microsoft Visual Studio.

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
