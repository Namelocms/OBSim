#include <thread>

#include "SimClock.h"
#include "CoreSim.h"

int main() {
	SimClock clock;
	CoreSim CS;
	CS.setParameters(
		1,			// Seed
		100,		// Initialization Ticks
		100,		// Agent Start Count
		250'000,	// Share Float
		1.00		// Start Price
	);
	CS.run(clock);
}