// OBSim_Server -- run the simulation and serve it over a loopback WebSocket.
//
// Headless. This is the process the webview desktop app will host in Phase 4, and it is
// also the whole browser and bot story: point anything at ws://127.0.0.1:8787 and it sees
// the same protocol.

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <string>
#include <thread>

#include "CoreSim.h"
#include "SimClock.h"
#include "Agent.h"          // TRANSIENT_DEFAULT_FRACTION
#include "SimSession.h"
#include "SimServer.h"
#include "UiServer.h"

int main(int argc, char** argv) {
	int port = 8787;
	int uiPort = 8788;
	bool serveUi = true;
	SimParams params;
	params.seed = 1;
	params.backDataDays = 1;
	params.liveStartSession = Session::REGULAR;
	params.minLiquidity = 0;
	params.agentCount = 500;
	params.shareFloat = 250'000;
	params.startPrice = 1.00;
	params.transientFraction = TRANSIENT_DEFAULT_FRACTION;

	// Deliberately minimal argument handling: every one of these is also settable at
	// runtime through the protocol's reset message, which is where a frontend will do it.
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		auto next = [&](unsigned int fallback) -> unsigned int {
			return (i + 1 < argc) ? (unsigned int)std::strtoul(argv[++i], nullptr, 10) : fallback;
			};
		if (arg == "--port")        { port = (int)next((unsigned int)port); }
		else if (arg == "--ui-port") { uiPort = (int)next((unsigned int)uiPort); }
		else if (arg == "--no-ui")  { serveUi = false; }
		else if (arg == "--seed")   { params.seed = next(params.seed); }
		else if (arg == "--days")   { params.backDataDays = next(params.backDataDays); }
		else if (arg == "--agents") { params.agentCount = next(params.agentCount); }
		else if (arg == "--float")  { params.shareFloat = next(params.shareFloat); }
		else if (arg == "--price" && i + 1 < argc) { params.startPrice = std::strtod(argv[++i], nullptr); }
		else if (arg == "--no-transients") { params.transientFraction = 0.0; }
		else if (arg == "--help") {
			std::printf("OBSim_Server [--port N] [--ui-port N] [--no-ui] [--seed N] [--days N]\n"
				"             [--agents N] [--float N] [--price X] [--no-transients]\n");
			return 0;
		}
	}

	SimClock clock;
	CoreSim sim;
	SimSession session(sim, clock);
	SimServer server(session, port);
	server.setParams(params);

	if (!server.start()) {
		std::printf("could not bind 127.0.0.1:%d -- is something already listening?\n", port);
		return 1;
	}
	// Serving the UI is optional. A bot, or a Vite dev server during development, wants the
	// socket and nothing else -- and a missing build must not stop the engine running.
	UiServer ui(serveUi ? findWebRoot(argv[0]) : std::string(), uiPort);
	if (serveUi) {
		if (ui.start()) {
			std::printf("OBSim UI at %s (from %s)\n", ui.url().c_str(), ui.root().c_str());
		} else {
			std::printf("no built UI found -- run `npm run build` in Web/, "
				"or use the Vite dev server\n");
		}
	}
	std::printf("OBSim serving ws://127.0.0.1:%d\n", port);
	std::printf("  seed %u, %u day(s) back data, %u agents, float %u, start $%.2f, transients %s\n",
		params.seed, params.backDataDays, params.agentCount, params.shareFloat,
		params.startPrice, params.transientFraction > 0.0 ? "on" : "off");
	std::fflush(stdout);

	session.start(params);

	// Nothing to do on this thread. The session owns the sim, the server owns its own
	// publisher, and IXWebSocket owns its accept loop.
	while (!session.finished()) {
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	}

	std::printf("simulation finished, shutting down\n");
	server.stop();
	ui.stop();
	session.stop();
	return 0;
}
