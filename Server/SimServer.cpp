#include "include/SimServer.h"

#include <chrono>
#include <thread>

#include <ixwebsocket/IXWebSocketServer.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXNetSystem.h>

SimServer::SimServer(SimSession& session, int port)
	: session_(session), port_(port) {
	// Winsock needs initialising once per process before any socket is created. IXWebSocket
	// wraps that; calling it twice is harmless, never calling it means nothing binds.
	ix::initNetSystem();
}

SimServer::~SimServer() {
	this->stop();
}

void SimServer::setParams(const SimParams& params) {
	std::lock_guard<std::mutex> lk(this->paramsMtx_);
	this->params_ = params;
}

bool SimServer::start() {
	if (this->running_.load()) { return true; }

	// 127.0.0.1, not 0.0.0.0. There is no authentication here and there should not need
	// to be one, which is only true while nothing off this machine can reach it.
	this->server_ = std::make_unique<ix::WebSocketServer>(this->port_, "127.0.0.1");

	this->server_->setOnClientMessageCallback(
		[this](std::shared_ptr<ix::ConnectionState> /*state*/,
			ix::WebSocket& webSocket,
			const ix::WebSocketMessagePtr& msg) {

				if (msg->type == ix::WebSocketMessageType::Open) {
					this->clients_.fetch_add(1);

					// hello first, always: a client cannot lay out a time axis without the
					// epoch base and the session lengths.
					SimParams params;
					{
						std::lock_guard<std::mutex> lk(this->paramsMtx_);
						params = this->params_;
					}
					webSocket.sendText(Protocol::encodeHello(params, this->session_.config));

					// Then ask for history on the new client's behalf. The fulfilment
					// happens on the sim thread and the publisher forwards it, so a client
					// gets its chart backfilled without ever touching the engine.
					this->session_.requestBackfill(this->defaultBackfillTrades);
					return;
				}

				if (msg->type == ix::WebSocketMessageType::Close
					|| msg->type == ix::WebSocketMessageType::Error) {
					if (msg->type == ix::WebSocketMessageType::Close) { this->clients_.fetch_sub(1); }
					return;
				}

				if (msg->type != ix::WebSocketMessageType::Message) { return; }

				// Everything arriving here is untrusted input from a page. decodeControl
				// never throws; a malformed message becomes an error reply.
				const Protocol::ControlMessage control = Protocol::decodeControl(msg->str);
				webSocket.sendText(this->handleControl_(control));
		});

	std::pair<bool, std::string> result = this->server_->listen();
	if (!result.first) {
		this->server_.reset();
		return false;
	}

	this->server_->start();
	this->listening_.store(true);
	this->running_.store(true);
	this->publisher_ = std::thread([this]() { this->publisherLoop_(); });
	return true;
}

void SimServer::stop() {
	if (!this->running_.exchange(false)) { return; }

	if (this->publisher_.joinable()) { this->publisher_.join(); }

	if (this->server_) {
		this->server_->stop();
		this->server_.reset();
	}
	this->listening_.store(false);
	this->clients_.store(0);
}

std::string SimServer::handleControl_(const Protocol::ControlMessage& msg) {
	using Protocol::Command;

	if (!msg.valid) {
		return Protocol::encodeError(
			msg.error.empty() ? "could not understand message" : msg.error, msg.id);
	}

	switch (msg.command) {
	case Command::Pause:
		this->session_.pause();
		return Protocol::encodeAck("pause", msg.id);
	case Command::Resume:
		this->session_.resume();
		return Protocol::encodeAck("resume", msg.id);
	case Command::Toggle:
		this->session_.togglePause();
		return Protocol::encodeAck("toggle", msg.id);
	case Command::Step:
		this->session_.step();
		return Protocol::encodeAck("step", msg.id);
	case Command::Speed:
		this->session_.setSpeed(msg.value);
		return Protocol::encodeAck("speed", msg.id);
	case Command::Sentiment:
		this->session_.setSentiment(msg.value);
		return Protocol::encodeAck("sentiment", msg.id);
	case Command::SentimentNudge:
		this->session_.nudgeSentiment(msg.value);
		return Protocol::encodeAck("sentimentNudge", msg.id);
	case Command::Backfill:
		this->session_.requestBackfill(msg.limit);
		return Protocol::encodeAck("backfill", msg.id);
	case Command::CancelBackData:
		this->session_.cancelBackData();
		return Protocol::encodeAck("cancelBackData", msg.id);
	case Command::Reset:
		// restart() joins the sim thread, which can take a moment while back data unwinds.
		// Done on the caller's connection thread deliberately: the client that asked is
		// the one that should wait for it, rather than the publisher stalling everyone.
		this->setParams(msg.params);
		this->session_.restart(msg.params);
		this->session_.requestBackfill(this->defaultBackfillTrades);
		return Protocol::encodeAck("reset", msg.id);
	default:
		return Protocol::encodeError("unhandled command", msg.id);
	}
}

void SimServer::publisherLoop_() {
	// Poll a little faster than frames are produced, so a frame is forwarded promptly
	// rather than waiting out most of an interval. The session is the rate limiter; this
	// thread only notices what it has already decided to publish.
	const auto pollInterval = std::chrono::milliseconds(5);

	while (this->running_.load()) {
		// Backfill first. A client that just connected should receive history before the
		// live frames that continue it, or its chart briefly draws a gap and then fills in
		// behind itself.
		TradeBackfill backfill;
		if (this->session_.takeBackfill(backfill)) {
			const std::string payload = Protocol::encodeBackfill(backfill);
			if (this->server_) {
				for (auto&& client : this->server_->getClients()) { client->sendText(payload); }
			}
		}

		MarketFrame frame;
		if (this->session_.latest(frame) && frame.sequence != this->lastSent_) {
			this->lastSent_ = frame.sequence;
			// Serialised once for everyone: the payload is identical per client, and JSON
			// encoding a frame with a full roster is the expensive part.
			const std::string payload = Protocol::encodeFrame(frame);
			if (this->server_) {
				for (auto&& client : this->server_->getClients()) { client->sendText(payload); }
			}
		}

		std::this_thread::sleep_for(pollInterval);
	}
}
