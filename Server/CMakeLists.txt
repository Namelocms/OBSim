# Server -- the loopback WebSocket transport, and the headless executable that runs it.
#
# This is where the dependencies live. Core and Bridge have none of them, so the engine
# and the publisher stay buildable (and WASM-compilable) without a socket.
# Broker presets: data, compiled in (OrderModelPlan Step 4.2). Bytes rather than a string
# literal, for MSVC's literal size cap; editing the JSON re-runs the configure step.
set(BROKER_PRESETS_JSON "${CMAKE_CURRENT_SOURCE_DIR}/presets/brokers.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${BROKER_PRESETS_JSON}")
file(READ "${BROKER_PRESETS_JSON}" presetsHex HEX)
string(LENGTH "${presetsHex}" presetsHexLength)
math(EXPR presetsBytes "${presetsHexLength} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," presetsArray "${presetsHex}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/BrokerPresetsData.cpp"
	"// Generated from presets/brokers.json by Server/CMakeLists.txt. Do not edit.\n"
	"#include <cstddef>\n"
	"extern const unsigned char BROKER_PRESETS_JSON[] = { ${presetsArray} 0x00 };\n"
	"extern const std::size_t BROKER_PRESETS_JSON_SIZE = ${presetsBytes};\n")

add_library(ObSimServer
	"Protocol.cpp"
	"Presets.cpp"
	"SimServer.cpp"
	"UiServer.cpp"
	"include/Protocol.h"
	"include/Presets.h"
	"include/SimServer.h"
	"include/UiServer.h"
	"presets/brokers.json"
	"${CMAKE_CURRENT_BINARY_DIR}/BrokerPresetsData.cpp"
)

target_include_directories(ObSimServer
	PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ObSimServer
	PUBLIC
		Bridge
		WebAssets
		nlohmann_json::nlohmann_json
		ixwebsocket
)

target_compile_features(ObSimServer PUBLIC cxx_std_20)

add_executable(OBSim_Server "main.cpp")
target_link_libraries(OBSim_Server PRIVATE ObSimServer)
