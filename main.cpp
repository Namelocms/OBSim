#include <iostream>
#include <chrono>
#include <vector>
#include <numeric>

#include "OrderBook.h"
#include "MatchingEngine.h"
#include "Agent.h"
#include "Enums.h"

void populateOrderBook(int numAgents, double startCash, OrderBook& ob, MatchingEngine& me) {
	for (int i = 0; i < numAgents; ++i) {
		std::shared_ptr<Agent> agent = std::make_shared<Agent>(
			ob.makeId(ID_TYPE::AGENT),
			startCash,
			AgentStatus::ACTIVE,
			ob,
			me
		);

		agent->upsertHolding(Holding(agent->getBetaPrice(ob.currentPrice, OrderAction::ASK), 10));
		agent->upsertHolding(Holding(agent->getBetaPrice(ob.currentPrice, OrderAction::BID), 10));

		ob.upsertAgent(agent);
	}
}

void info(OrderBook& OB) {
	for (auto& kv : OB.agents) {
		std::shared_ptr<Agent> agent = kv.second;
		std::string infoString = "ID: " + agent->id +
			"\nCash: " + std::to_string(agent->cash) +
			"\nHoldings:";
		for (const auto& pair : agent->holdings) {
			infoString += ("\n__Price: " + std::to_string(pair.second.price) + ": " + std::to_string(pair.second.volume));
		}

		std::cout << infoString << "\n===========================================================" << std::endl;
	}
}

int main() {
    const int NUM_RUNS = 10;        // How many times to repeat the whole test
    const int STEPS = 1440;         // Steps per simulation
    const int NUM_AGENTS = 1000;
    const double START_PRICE = 1.00;

    std::vector<double> runTimes;
    runTimes.reserve(NUM_RUNS);

    for (int r = 0; r < NUM_RUNS; ++r) {
        // --- Setup (not timed) ---
        OrderBook OB = OrderBook(START_PRICE);
        MatchingEngine ME = MatchingEngine(OB);
        populateOrderBook(NUM_AGENTS, 100.00, OB, ME);

        // --- Start Timing ---
        auto start = std::chrono::steady_clock::now();

        for (int x = 0; x < STEPS; ++x) {
            for (const auto& kv : OB.agents) {
                kv.second->actRandom();
            }
        }

        auto end = std::chrono::steady_clock::now();
        // --- End Timing ---

        std::chrono::duration<double, std::milli> elapsed = end - start;
        runTimes.push_back(elapsed.count());

        std::cout << "| Run " << r + 1 << " | " << elapsed.count() << " ms |" << std::endl;
    }

    // --- Calculate Stats ---
    double totalTime = std::accumulate(runTimes.begin(), runTimes.end(), 0.0);
    double averageTime = totalTime / NUM_RUNS;

    std::cout << "\n--- Final Results ---" << std::endl;
    std::cout << "Total Runs:    " << NUM_RUNS << std::endl;
    std::cout << "Steps per Run: " << STEPS << std::endl;
    std::cout << "Agent Count:   " << NUM_AGENTS << std::endl;
    std::cout << "Average Time:  " << averageTime << " ms" << std::endl;
    std::cout << "Throughput:    " << (STEPS * NUM_AGENTS / (averageTime / 1000.0)) << " actions/sec" << std::endl;

    return 0;
}
