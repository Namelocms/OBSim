#include "include/UiServer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <ixwebsocket/IXHttpServer.h>

namespace fs = std::filesystem;

namespace {

/* Media types for what a Vite build actually emits. Deliberately a short list: an unknown
*  extension gets application/octet-stream rather than a guess, because a wrong type on a
*  script or stylesheet fails in ways that are tedious to diagnose. */
std::string contentTypeFor(const std::string& extension) {
	if (extension == ".html") { return "text/html; charset=utf-8"; }
	if (extension == ".js" || extension == ".mjs") { return "text/javascript; charset=utf-8"; }
	if (extension == ".css") { return "text/css; charset=utf-8"; }
	if (extension == ".json") { return "application/json; charset=utf-8"; }
	if (extension == ".svg") { return "image/svg+xml"; }
	if (extension == ".png") { return "image/png"; }
	if (extension == ".jpg" || extension == ".jpeg") { return "image/jpeg"; }
	if (extension == ".ico") { return "image/x-icon"; }
	if (extension == ".woff2") { return "font/woff2"; }
	if (extension == ".woff") { return "font/woff"; }
	if (extension == ".map") { return "application/json; charset=utf-8"; }
	return "application/octet-stream";
}

/* Resolve a request path inside root, refusing anything that escapes it
*
* This server binds loopback and serves our own build, so a traversal attempt is not the
* expected case -- but "only we can reach it" is an argument that stops being true the
* first time someone changes the bind address, and a containment check costs nothing.
*/
bool resolveWithin(const fs::path& root, const std::string& requestPath, fs::path& out) {
	std::string relative = requestPath;
	const size_t query = relative.find('?');
	if (query != std::string::npos) { relative = relative.substr(0, query); }
	if (relative.empty() || relative == "/") { relative = "/index.html"; }
	while (!relative.empty() && relative.front() == '/') { relative.erase(relative.begin()); }
	if (relative.empty()) { relative = "index.html"; }

	std::error_code ec;
	const fs::path candidate = fs::weakly_canonical(root / fs::path(relative), ec);
	if (ec) { return false; }
	const fs::path canonicalRoot = fs::weakly_canonical(root, ec);
	if (ec) { return false; }

	// mismatch on the root's end means the candidate climbed out of it
	const auto rootStr = canonicalRoot.generic_string();
	const auto candidateStr = candidate.generic_string();
	if (candidateStr.size() < rootStr.size()
		|| candidateStr.compare(0, rootStr.size(), rootStr) != 0) {
		return false;
	}

	out = candidate;
	return true;
}

} // namespace

UiServer::UiServer(std::string root, int port)
	: root_(std::move(root)), port_(port) {}

UiServer::~UiServer() {
	this->stop();
}

std::string UiServer::url() const {
	return "http://127.0.0.1:" + std::to_string(this->port_) + "/";
}

bool UiServer::start() {
	if (this->running_) { return true; }

	std::error_code ec;
	if (this->root_.empty() || !fs::is_directory(this->root_, ec)) { return false; }

	this->server_ = std::make_unique<ix::HttpServer>(this->port_, "127.0.0.1");

	const fs::path root = this->root_;
	this->server_->setOnConnectionCallback(
		[root](ix::HttpRequestPtr request,
			std::shared_ptr<ix::ConnectionState> /*state*/) -> ix::HttpResponsePtr {

				ix::WebSocketHttpHeaders headers;
				headers["Server"] = "OBSim";

				if (request->method != "GET" && request->method != "HEAD") {
					return std::make_shared<ix::HttpResponse>(
						405, "Method Not Allowed", ix::HttpErrorCode::Ok, headers, "");
				}

				fs::path file;
				if (!resolveWithin(root, request->uri, file)) {
					return std::make_shared<ix::HttpResponse>(
						403, "Forbidden", ix::HttpErrorCode::Ok, headers, "");
				}

				std::error_code fileEc;
				if (fs::is_directory(file, fileEc)) { file /= "index.html"; }
				if (!fs::is_regular_file(file, fileEc)) {
					return std::make_shared<ix::HttpResponse>(
						404, "Not Found", ix::HttpErrorCode::Ok, headers, "");
				}

				std::ifstream in(file, std::ios::binary);
				if (!in) {
					return std::make_shared<ix::HttpResponse>(
						500, "Internal Server Error", ix::HttpErrorCode::Ok, headers, "");
				}
				std::ostringstream body;
				body << in.rdbuf();

				headers["Content-Type"] = contentTypeFor(file.extension().string());
				// Vite fingerprints asset filenames, so assets can be cached hard while the
				// entry document must not be -- otherwise a rebuilt UI keeps serving the
				// previous bundle from the webview's cache.
				const bool fingerprinted = file.filename().string().find('-') != std::string::npos
					&& file.extension() != ".html";
				headers["Cache-Control"] = fingerprinted
					? "public, max-age=31536000, immutable"
					: "no-store";

				return std::make_shared<ix::HttpResponse>(
					200, "OK", ix::HttpErrorCode::Ok, headers, body.str());
		});

	std::pair<bool, std::string> result = this->server_->listen();
	if (!result.first) {
		this->server_.reset();
		return false;
	}
	this->server_->start();
	this->running_ = true;
	return true;
}

void UiServer::stop() {
	if (!this->running_) { return; }
	if (this->server_) {
		this->server_->stop();
		this->server_.reset();
	}
	this->running_ = false;
}

std::string findWebRoot(const std::string& exePath) {
	std::error_code ec;
	const fs::path exe = fs::path(exePath);
	fs::path dir = exe.has_parent_path() ? exe.parent_path() : fs::current_path(ec);

	// Beside the executable is where a packaged build puts it. Walking up to the source
	// tree's Web/dist is for running straight out of the build directory, which is what
	// happens every time during development.
	const char* const candidates[] = { "web", "Web/dist", "../Web/dist" };

	for (int levels = 0; levels < 6 && !dir.empty(); ++levels) {
		for (const char* candidate : candidates) {
			const fs::path probe = dir / candidate;
			if (fs::is_regular_file(probe / "index.html", ec)) {
				return fs::weakly_canonical(probe, ec).string();
			}
		}
		if (!dir.has_parent_path() || dir.parent_path() == dir) { break; }
		dir = dir.parent_path();
	}
	return {};
}
