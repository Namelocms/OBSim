# Bridge -- the seam between the engine and any out-of-thread frontend.
#
# Links Simulator and nothing else. No FTXUI, no webview, no transport: those belong to
# whoever consumes a MarketFrame, not to the thing that produces one.
add_library(Bridge
	"SimSession.cpp"
	"include/SimSession.h"
	"include/MarketFrame.h"
)

target_include_directories(Bridge
	PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(Bridge
	PUBLIC
		Simulator
)

target_compile_features(Bridge
	PUBLIC
		cxx_std_20
)
