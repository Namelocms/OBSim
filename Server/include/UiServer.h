#pragma once
#include <memory>
#include <string>

namespace ix { class HttpServer; }

/* ---- Serving the built web UI over loopback HTTP ----
*
* The desktop shell could load the bundle straight off disk with a file:// URL, and it
* would nearly work. This exists because "nearly" is the problem: a file:// page is an
* opaque origin, which makes every future decision about storage, workers and fetch a
* special case, and it differs from what a browser sees -- so the desktop app would be
* testing a subtly different thing from the browser client.
*
* Serving it instead means the webview and a real browser load byte-identical pages from
* the same origin, which is the whole point of having chosen a socket over a direct
* binding. Loopback only, same as the WebSocket server.
*/
class UiServer {
public:
	/* `root` is the directory to serve, normally Web/dist */
	UiServer(std::string root, int port = 8788);
	~UiServer();

	UiServer(const UiServer&) = delete;
	UiServer& operator=(const UiServer&) = delete;

	/* False when the port could not be bound or the root does not exist */
	bool start();
	void stop();

	int port() const { return this->port_; }
	const std::string& root() const { return this->root_; }
	std::string url() const;

private:
	std::string root_;
	int port_;
	std::unique_ptr<ix::HttpServer> server_;
	bool running_ = false;
};

/* Find the built UI next to the executable, or in the source tree during development
*
* Returns an empty string when no build is found, which the caller should treat as "run
* `npm run build` first" rather than as a crash.
*/
std::string findWebRoot(const std::string& exePath);
