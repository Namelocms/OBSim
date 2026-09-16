# OBSim_App -- the desktop product.
#
# The same engine, the same server, the same page a browser would load. The only things
# this target adds are a window and a WIN32 entry point. That thinness is deliberate:
# anything it could do that the browser client cannot would be a divergence.
#
# WIN32 puts it on the GUI subsystem so launching it does not flash up a console. The cost
# is that nothing can report a problem by printing -- see fail() in main.cpp.
add_executable(OBSim_App WIN32 "main.cpp")

target_link_libraries(OBSim_App
	PRIVATE
		ObSimServer
		webview::core
)

target_compile_features(OBSim_App PRIVATE cxx_std_20)

# No DLL to ship: webview::core is the header-only interface target, so the implementation
# compiles into this executable and WebView2's loader links statically. The web UI is
# compiled in via WebAssets, which ObSimServer already carries. One file, nothing beside it.
