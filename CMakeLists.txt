cmake_minimum_required(VERSION 3.31)

project(OBSim)

include(FetchContent)

FetchContent_Declare(ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
  GIT_TAG v6.1.9
)

FetchContent_MakeAvailable(ftxui)
message(STATUS "FTXUI source dir: ${ftxui_SOURCE_DIR}")

# ---- Web frontend dependencies ----
#
# Used only by the Server target. Core and Bridge stay free of them, so the engine and the
# publisher can be built (or compiled to WebAssembly later) without a socket in sight.

FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json
  GIT_TAG v3.12.0
)

# Loopback only, so no TLS and no compression. Nothing here is reachable from off the
# machine, and permessage-deflate would only trade CPU for bandwidth that costs nothing
# over a socket to ourselves -- it also drags in zlib, which Windows has no system copy of.
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
set(USE_TLS OFF CACHE BOOL "" FORCE)
set(USE_ZLIB OFF CACHE BOOL "" FORCE)
FetchContent_Declare(ixwebsocket
  GIT_REPOSITORY https://github.com/machinezone/IXWebSocket
  GIT_TAG v12.0.1
)

# The desktop shell. Uses the system webview -- WebView2 on Windows, WKWebView on macOS,
# WebKitGTK on Linux -- so the binary stays small and there is no bundled Chromium.
set(WEBVIEW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(webview
  GIT_REPOSITORY https://github.com/webview/webview
  GIT_TAG 0.12.0
)

FetchContent_MakeAvailable(nlohmann_json ixwebsocket webview)

add_subdirectory(Core)
add_subdirectory(Bridge)
add_subdirectory(Server)
add_subdirectory(Desktop)
add_subdirectory(App)
