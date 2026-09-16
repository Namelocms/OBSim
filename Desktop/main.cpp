// OBSim_App -- the desktop application.
//
// Starts the engine, serves the built UI over loopback, and opens a WebView2 window on it.
// Nothing else. The page it loads is byte-identical to the one a browser gets, which is
// the point of having chosen a socket over a direct JS binding: the desktop app is not a
// different product with different capabilities, it is the same client in a window.

#include <cstdio>
#include <chrono>
#include <string>
#include <thread>

#include "webview/webview.h"

#include "CoreSim.h"
#include "SimClock.h"
#include "Agent.h"          // TRANSIENT_DEFAULT_FRACTION
#include "SimSession.h"
#include "SimServer.h"
#include "UiServer.h"

int main(int argc, char** argv) {
	int port = 8787;
	int uiPort = 8788;

	SimParams params;
	params.seed = 1;
	params.backDataDays = 1;
	params.liveStartSession = Session::REGULAR;
	params.agentCount = 500;
	params.shareFloat = 250'000;
	params.startPrice = 1.00;
	params.transientFraction = TRANSIENT_DEFAULT_FRACTION;

	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		auto next = [&](unsigned int fallback) -> unsigned int {
			return (i + 1 < argc) ? (unsigned int)std::strtoul(argv[++i], nullptr, 10) : fallback;
			};
		if (arg == "--port")         { port = (int)next((unsigned int)port); }
		else if (arg == "--ui-port") { uiPort = (int)next((unsigned int)uiPort); }
		else if (arg == "--seed")    { params.seed = next(params.seed); }
		else if (arg == "--days")    { params.backDataDays = next(params.backDataDays); }
		else if (arg == "--agents")  { params.agentCount = next(params.agentCount); }
		else if (arg == "--float")   { params.shareFloat = next(params.shareFloat); }
		else if (arg == "--no-transients") { params.transientFraction = 0.0; }
	}

	const std::string webRoot = findWebRoot(argv[0]);
	if (webRoot.empty()) {
		std::printf("no built UI found. Run `npm install && npm run build` in Web/ first.\n");
		return 1;
	}

	SimClock clock;
	CoreSim sim;
	SimSession session(sim, clock);
	SimServer server(session, port);
	UiServer ui(webRoot, uiPort);

	server.setParams(params);
	if (!server.start()) {
		std::printf("could not bind ws://127.0.0.1:%d\n", port);
		return 1;
	}
	if (!ui.start()) {
		std::printf("could not serve the UI on port %d\n", uiPort);
		server.stop();
		return 1;
	}
	session.start(params);

	{
		// Scoped so the window is destroyed before the servers it talks to. webview::run
		// owns the main thread until the window closes, which is why everything else had
		// to be started already.
		webview::webview window(false, nullptr);
		window.set_title("OBSim");
		window.set_size(1500, 940, WEBVIEW_HINT_NONE);
		window.set_size(900, 600, WEBVIEW_HINT_MIN);
		window.navigate(ui.url() + "?server=127.0.0.1:" + std::to_string(port));
		window.run();
	}

	server.stop();
	ui.stop();
	session.stop();

	// The sim thread is joined by session.stop(); this is only so a slow unwind does not
	// race the process exiting while IXWebSocket's own threads are still winding down.
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	return 0;
}
