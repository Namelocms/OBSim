// OBSim_App -- the desktop application.
//
// Starts the engine, serves the UI compiled into this binary over loopback, and opens a
// WebView2 window on it. One executable, nothing beside it.
//
// The page it loads is byte-identical to the one a browser gets from OBSim_Server, over
// the same WebSocket. That is the point of having chosen a socket over a direct JS
// binding: the desktop app is not a different product with different capabilities, it is
// the same client in a window.
//
// Built for the WIN32 subsystem, so there is no console. Nothing may report a problem by
// printing -- see fail().

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

#include "webview/webview.h"

#include "CoreSim.h"
#include "SimClock.h"
#include "Agent.h"          // TRANSIENT_DEFAULT_FRACTION
#include "SimSession.h"
#include "SimServer.h"
#include "UiServer.h"

namespace {

/* Report a startup failure to a user who has no console to read
*
* A GUI-subsystem process that writes to stdout writes into the void, so a failure before
* the window exists would otherwise look like the app simply not starting.
*/
void fail(const std::string& message) {
#ifdef _WIN32
	MessageBoxA(nullptr, message.c_str(), "OBSim", MB_OK | MB_ICONERROR);
#else
	std::fputs((message + "\n").c_str(), stderr);
#endif
}

int runApp(int argc, char** argv) {
	int port = 8787;
	int uiPort = 8788;
	std::string webRoot;   // empty serves the bundle compiled into this binary

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
		else if (arg == "--web-root") {
			webRoot = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : findWebRoot(argv[0]);
		}
	}

	SimClock clock;
	CoreSim sim;
	SimSession session(sim, clock);
	SimServer server(session, port);
	UiServer ui(uiPort, webRoot);

	if (!ui.hasContent()) {
		fail("This build has no web UI compiled into it.\n\n"
			"Reconfigure with npm available, or start it with --web-root pointing at a "
			"built Web/dist.");
		return 1;
	}

	server.setParams(params);
	if (!server.start()) {
		fail("Could not bind ws://127.0.0.1:" + std::to_string(port)
			+ "\n\nAnother OBSim may already be running. Try --port.");
		return 1;
	}
	if (!ui.start()) {
		fail("Could not serve the UI on port " + std::to_string(uiPort)
			+ "\n\nTry --ui-port.");
		server.stop();
		return 1;
	}

	session.start(params);

	{
		// Scoped so the window is destroyed before the servers it talks to. webview::run
		// owns this thread until the window closes, which is why everything else had to
		// be started already.
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

	// session.stop() joins the sim thread; this is only so a slow unwind does not race the
	// process exiting while IXWebSocket's own threads are still winding down.
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	return 0;
}

} // namespace

#ifdef _WIN32
/* WIN32 subsystem entry point, so launching the app does not flash up a console window.
*
* __argc / __argv are the CRT's already-parsed command line, so the argument handling
* above stays identical to every other target's rather than needing a Windows-only path.
*/
int APIENTRY WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
	return runApp(__argc, __argv);
}
#else
int main(int argc, char** argv) {
	return runApp(argc, argv);
}
#endif
