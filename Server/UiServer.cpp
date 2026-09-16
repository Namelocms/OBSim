#include "include/UiServer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <ixwebsocket/IXHttpServer.h>

#include "EmbeddedWeb.h"

namespace fs = std::filesystem;

namespace {

/* Media types for what a Vite build actually emits. Deliberately a short list: an unknown
*  extension gets application/octet-stream rather than a guess, because a wrong type on a
*  script or stylesheet fails in ways that are tedious to diagnose. */
std::string contentTypeFor(const std::string& extension) {
	if (extension == ".html") { return "text/html; charset=utf-8"; }
	if (extension == ".js" || extension == ".mjs") { return "text/javascript; charset=utf-8"; }
	if (extension == ".css") { return "text/css; charset=utf-8"; }
	if (extension == ".json" || extension == ".map") { return "application/json; charset=utf-8"; }
	if (extension == ".svg") { return "image/svg+xml"; }
	if (extension == ".png") { return "image/png"; }
	if (extension == ".jpg" || extension == ".jpeg") { return "image/jpeg"; }
	if (extension == ".ico") { return "image/x-icon"; }
	if (extension == ".woff2") { return "font/woff2"; }
	if (extension == ".woff") { return "font/woff"; }
	return "application/octet-stream";
}

/* Strip the query string and leading slashes, and turn "/" into the index document */
std::string normalisePath(const std::string& uri) {
	std::string path = uri;
	const size_t query = path.find('?');
	if (query != std::string::npos) { path = path.substr(0, query); }
	const size_t fragment = path.find('#');
	if (fragment != std::string::npos) { path = path.substr(0, fragment); }
	while (!path.empty() && path.front() == '/') { path.erase(path.begin()); }
	if (path.empty()) { path = "index.html"; }
	return path;
}

std::string extensionOf(const std::string& path) {
	const size_t slash = path.find_last_of('/');
	const size_t dot = path.find_last_of('.');
	if (dot == std::string::npos) { return {}; }
	if (slash != std::string::npos && dot < slash) { return {}; }
	return path.substr(dot);
}

/* Vite fingerprints asset filenames, so assets can be cached hard while the entry document
*  must not be -- otherwise a rebuilt UI keeps serving the previous bundle out of the
*  webview's cache, which presents as "my change did nothing". */
std::string cacheControlFor(const std::string& path) {
	const std::string extension = extensionOf(path);
	const bool fingerprinted = extension != ".html" && path.find('-') != std::string::npos;
	return fingerprinted ? "public, max-age=31536000, immutable" : "no-store";
}

const EmbeddedAsset* findEmbedded(const std::string& path) {
	size_t count = 0;
	const EmbeddedAsset* assets = embeddedWebAssets(count);
	for (size_t i = 0; i < count; ++i) {
		if (assets[i].path != nullptr && path == assets[i].path) { return &assets[i]; }
	}
	return nullptr;
}

bool anyEmbedded() {
	size_t count = 0;
	embeddedWebAssets(count);
	return count > 0;
}

/* Resolve a request path inside root, refusing anything that escapes it
*
* This server binds loopback and serves our own build, so a traversal attempt is not the
* expected case -- but "only we can reach it" is an argument that stops being true the
* first time someone changes the bind address, and a containment check costs nothing.
*/
bool resolveWithin(const fs::path& root, const std::string& relative, fs::path& out) {
	std::error_code ec;
	const fs::path candidate = fs::weakly_canonical(root / fs::path(relative), ec);
	if (ec) { return false; }
	const fs::path canonicalRoot = fs::weakly_canonical(root, ec);
	if (ec) { return false; }

	// A candidate that does not start with the root climbed out of it
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

UiServer::UiServer(int port, std::string diskRoot)
	: port_(port), diskRoot_(std::move(diskRoot)) {}

UiServer::~UiServer() {
	this->stop();
}

std::string UiServer::url() const {
	return "http://127.0.0.1:" + std::to_string(this->port_) + "/";
}

bool UiServer::hasContent() const {
	if (!this->diskRoot_.empty()) {
		std::error_code ec;
		return fs::is_directory(this->diskRoot_, ec);
	}
	return anyEmbedded();
}

std::string UiServer::sourceDescription() const {
	if (!this->diskRoot_.empty()) { return this->diskRoot_; }
	return anyEmbedded() ? "embedded" : "nothing";
}

bool UiServer::start() {
	if (this->running_) { return true; }
	if (!this->hasContent()) { return false; }

	this->server_ = std::make_unique<ix::HttpServer>(this->port_, "127.0.0.1");

	const std::string diskRoot = this->diskRoot_;
	this->server_->setOnConnectionCallback(
		[diskRoot](ix::HttpRequestPtr request,
			std::shared_ptr<ix::ConnectionState> /*state*/) -> ix::HttpResponsePtr {

				ix::WebSocketHttpHeaders headers;
				headers["Server"] = "OBSim";

				if (request->method != "GET" && request->method != "HEAD") {
					return std::make_shared<ix::HttpResponse>(
						405, "Method Not Allowed", ix::HttpErrorCode::Ok, headers, "");
				}

				const std::string path = normalisePath(request->uri);

				std::string body;
				if (diskRoot.empty()) {
					const EmbeddedAsset* asset = findEmbedded(path);
					if (asset == nullptr) {
						return std::make_shared<ix::HttpResponse>(
							404, "Not Found", ix::HttpErrorCode::Ok, headers, "");
					}
					body.assign(reinterpret_cast<const char*>(asset->data), asset->size);
				}
				else {
					fs::path file;
					if (!resolveWithin(diskRoot, path, file)) {
						return std::make_shared<ix::HttpResponse>(
							403, "Forbidden", ix::HttpErrorCode::Ok, headers, "");
					}
					std::error_code ec;
					if (fs::is_directory(file, ec)) { file /= "index.html"; }
					if (!fs::is_regular_file(file, ec)) {
						return std::make_shared<ix::HttpResponse>(
							404, "Not Found", ix::HttpErrorCode::Ok, headers, "");
					}
					std::ifstream in(file, std::ios::binary);
					if (!in) {
						return std::make_shared<ix::HttpResponse>(
							500, "Internal Server Error", ix::HttpErrorCode::Ok, headers, "");
					}
					std::ostringstream buffer;
					buffer << in.rdbuf();
					body = buffer.str();
				}

				headers["Content-Type"] = contentTypeFor(extensionOf(path));
				headers["Cache-Control"] = cacheControlFor(path);

				return std::make_shared<ix::HttpResponse>(
					200, "OK", ix::HttpErrorCode::Ok, headers, body);
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

	// Beside the executable first, then up through the build tree to the source tree's
	// own build output -- which is where it sits when running straight out of a build
	// directory during development.
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
