#include "SimClock.h"
#include "CoreSim.h"
#include "include/SimTUI.h"

int main() {
	SimClock clock;
	CoreSim sim;
	sim.setParameters(
		1,			// Seed
		1,		// Initialization Ticks
		100,		// Agent Start Count
		2'500'000,	// Share Float
		1.00		// Start Price
	);
	
	SimTUI tui(sim, clock);
	tui.run();

	return 0;
}