# OBSim_App -- the desktop product.
#
# The same engine, the same server, the same page a browser would load. The only thing
# this target adds is a window: it starts the simulation, serves the UI over loopback, and
# points an embedded WebView2 at it. That is the whole shell, and it is deliberately thin
# -- anything it could do that the browser client cannot would be a divergence.
add_executable(OBSim_App "main.cpp")

target_link_libraries(OBSim_App
	PRIVATE
		ObSimServer
		webview::core
)

target_compile_features(OBSim_App PRIVATE cxx_std_20)


# No DLL to ship: webview::core is the header-only interface target, so the
# implementation compiles into this executable and WebView2's loader links statically.
# webview_core_shared exists in the build tree but nothing here uses it.
