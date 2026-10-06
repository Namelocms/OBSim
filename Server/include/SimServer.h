#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "SimSession.h"
#include "Protocol.h"

namespace ix { class WebSocketServer; }

/* ---- Serving a run over a loopback WebSocket ----
*
* One protocol, three clients: the webview desktop app, a browser, and anything scripted.
* That is the whole reason this is a socket rather than a direct binding into a webview --
* a binding would have served exactly one of them.
*
* Binds 127.0.0.1 only. Nothing here is reachable from off the machine, which is also why
* there is no TLS and no authentication: adding either would imply a threat model that
* does not apply, and would need revisiting properly if it ever did.
*
* Threading. The session publishes frames on the SIM thread. This class runs its own
* publisher thread that copies the newest frame, serialises it, and broadcasts -- so JSON
* encoding never happens on the sim thread, where it would slow the simulation itself.
* IXWebSocket runs its own accept and per-connection threads on top of that.
*/
class SimServer {
public:
	SimServer(SimSession& session, int port = 8787);
	~SimServer();

	SimServer(const SimServer&) = delete;
	SimServer& operator=(const SimServer&) = delete;

	/* Start listening and begin broadcasting. False if the port could not be bound. */
	bool start();
	/* Stop broadcasting and close every connection. Safe if never started. */
	void stop();

	bool listening() const { return this->listening_.load(); }
	int port() const { return this->port_; }
	/* Connected clients right now */
	int clients() const { return this->clients_.load(); }

	/* Backfill depth offered to a client that does not ask for a specific one */
	size_t defaultBackfillTrades = 100000;
	/* Unsent bytes past which a client misses frames until it catches up: about one frame with a
	*  full roster, so a reply queued behind it waits a fraction of a second even for a slow reader */
	static constexpr size_t MAX_CLIENT_BACKLOG_BYTES = 128 * 1024;

private:
	void publisherLoop_();
	/* Apply one decoded control message, returning the reply to send back */
	std::string handleControl_(const Protocol::ControlMessage& msg);

	SimSession& session_;
	int port_;

	std::unique_ptr<ix::WebSocketServer> server_;
	std::thread publisher_;
	std::atomic<bool> running_{ false };
	std::atomic<bool> listening_{ false };
	std::atomic<int> clients_{ 0 };

	/* Parameters of the run currently being served, for `hello`
	*
	* Kept here rather than read back off the session because a reset replaces them and a
	* client connecting afterwards must be told what is actually running.
	*/
	std::mutex paramsMtx_;
	SimParams params_;

	/* Sequence of the last frame broadcast, so the publisher does not resend one */
	unsigned long long lastSent_ = 0;

public:
	/* Record the parameters a run was started with, so `hello` can report them */
	void setParams(const SimParams& params);
};
