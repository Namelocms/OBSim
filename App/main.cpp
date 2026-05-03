#include "SimClock.h"
#include "CoreSim.h"
#include "include/SimTUI.h"

int main() {
	SimClock clock;
	CoreSim sim;
	sim.setParameters(
		1,			// Seed
		100,		// Initialization Ticks
		1000,		// Agent Start Count
		250'000,	// Share Float
		1.00		// Start Price
	);

	SimTUI tui(sim, clock);
	tui.run();


	// Reset freezes on dialog screen after pressing enter

	return 0;
}