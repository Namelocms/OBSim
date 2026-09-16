#pragma once
#include <memory>
#include <string>

namespace ix { class HttpServer; }

/* ---- Serving the web UI over loopback HTTP ----
*
* Serves the bundle COMPILED INTO the binary by default, so the executable is
* self-contained: no dist/ folder beside it, and nothing that can get out of step with the
* build. Pass a directory to override that and serve from disk instead, which is what a
* developer wants when iterating on the UI without a full rebuild.
*
* The desktop shell could have loaded the bundle from a file:// URL rather than serving it
* at all. This exists because "nearly works" is the problem: a file:// page is an opaque
* origin, which makes every later decision about storage, workers and fetch a special
* case, and it differs from what a browser sees -- so the desktop app would be testing a
* subtly different thing from the browser client. Serving means the webview and a real
* browser load byte-identical pages from the same origin.
*
* Loopback only, same as the WebSocket server.
*/
class UiServer {
public:
	/* `diskRoot` empty (the default) serves the embedded bundle; otherwise that directory */
	explicit UiServer(int port = 8788, std::string diskRoot = {});
	~UiServer();

	UiServer(const UiServer&) = delete;
	UiServer& operator=(const UiServer&) = delete;

	/* False when the port could not be bound, or when there is nothing at all to serve */
	bool start();
	void stop();

	int port() const { return this->port_; }
	std::string url() const;

	/* Whether a UI is available to serve, embedded or on disk
	*
	* False is a legitimate state: a build without npm carries no UI, and the engine is
	* still perfectly useful over its socket.
	*/
	bool hasContent() const;
	/* Where the content is coming from, for logging: a path, or "embedded" */
	std::string sourceDescription() const;

private:
	int port_;
	std::string diskRoot_;
	std::unique_ptr<ix::HttpServer> server_;
	bool running_ = false;
};

/* Find a built UI on disk, for --web-root and for development
*
* Returns an empty string when none is found. Only needed to override the embedded bundle;
* the app does not call this unless asked to.
*/
std::string findWebRoot(const std::string& exePath);
