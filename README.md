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
*   **Standard Library:** Heavy use of `<chrono>` for timing, `<memory>` for smart pointers, and `<set>` for sorted order management.

### Prerequisites

- CMake >= 3.31
- A C++ compiler (Visual Studio 2022 or later on Windows, or GCC/Clang on Linux/macOS)

## Usage Example
Look at `App/main.cpp` for the most up-to-date usage.

## Benchmarks
For detailed performance benchmarks, look at the `Benchmarks.md` file
