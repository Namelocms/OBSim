#include <thread>

#include "SimClock.h"
#include "CoreSim.h"

int main() {
	SimClock clock;
	CoreSim CS;
	CS.setParameters();
	CS.run(clock);

	std::cout << "OK" << std::endl;
}