#include "SimClock.h"
#include "CoreSim.h"
#include "include/SimTUI.h"

int main() {
	SimClock clock;
	CoreSim sim;
	sim.setParameters(
		1,					// Seed
		1,					// Back Data (whole days)
		Session::REGULAR,	// Live Start Session, live sim begins at this session's open
		0,					// Min Liquidity, 0 disables the check
		100,				// Agent Start Count
		2'500'000,			// Share Float
		1.00				// Start Price, price at the START of the back data
	);
	
	SimTUI tui(sim, clock);
	tui.run();

	return 0;
}