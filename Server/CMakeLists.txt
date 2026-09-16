# Server -- the loopback WebSocket transport, and the headless executable that runs it.
#
# This is where the dependencies live. Core and Bridge have none of them, so the engine
# and the publisher stay buildable (and WASM-compilable) without a socket.
add_library(ObSimServer
	"Protocol.cpp"
	"SimServer.cpp"
	"UiServer.cpp"
	"include/Protocol.h"
	"include/SimServer.h"
	"include/UiServer.h"
)

target_include_directories(ObSimServer
	PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ObSimServer
	PUBLIC
		Bridge
		nlohmann_json::nlohmann_json
		ixwebsocket
)

target_compile_features(ObSimServer PUBLIC cxx_std_20)

add_executable(OBSim_Server "main.cpp")
target_link_libraries(OBSim_Server PRIVATE ObSimServer)
