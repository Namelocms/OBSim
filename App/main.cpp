#include <iostream>
#include <chrono>
#include <thread>

#include "OrderBook.h"
#include "MatchingEngine.h"
#include "Agent.h"
#include "Holding.h"
#include "Enums.h"
#include "Util.h"

#define NOW std::chrono::high_resolution_clock::now()

int main() {
	bool isRunning = true;

	const double START_PRICE = 1.00;
	std::cout << "OK" << std::endl;
}