# WebAssets -- build the UI with npm, then compile it into the binary.
#
# The result is that OBSim_App and OBSim_Server carry their own UI: one executable, no
# dist/ folder to ship beside it, nothing to get out of step with the build.
#
# Turning OBSIM_BUILD_WEB off, or simply not having npm, is a supported configuration. It
# produces an empty asset table: everything still builds, the engine still runs and serves
# its socket, and the app reports that no UI was compiled in. That matters because the
# engine is useful headless -- a bot does not need a chart.

option(OBSIM_BUILD_WEB "Build the web UI with npm and embed it in the binaries" ON)

set(WEB_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(WEB_DIST_DIR "${CMAKE_CURRENT_BINARY_DIR}/dist")
set(WEB_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/EmbeddedWeb.cpp")
set(EMBED_SCRIPT "${CMAKE_SOURCE_DIR}/cmake/EmbedWebAssets.cmake")

find_program(NPM_EXECUTABLE NAMES npm.cmd npm)

if(OBSIM_BUILD_WEB AND NOT NPM_EXECUTABLE)
	message(WARNING
		"npm not found: the web UI will not be built and the binaries will carry no UI. "
		"Install Node, or configure with -DOBSIM_BUILD_WEB=OFF to silence this.")
endif()

if(OBSIM_BUILD_WEB AND NPM_EXECUTABLE)
	# Re-glob on every build so a new source file is noticed without re-running CMake by hand
	file(GLOB_RECURSE WEB_SOURCES CONFIGURE_DEPENDS
		"${WEB_SOURCE_DIR}/src/*.ts"
		"${WEB_SOURCE_DIR}/src/*.css"
	)
	list(APPEND WEB_SOURCES
		"${WEB_SOURCE_DIR}/index.html"
		"${WEB_SOURCE_DIR}/vite.config.ts"
		"${WEB_SOURCE_DIR}/tsconfig.json"
	)

	# Dependencies first, and only when the manifests move. `npm ci` rather than `install`
	# so the lockfile is honoured exactly -- a build that silently resolves a different
	# dependency tree than the lockfile describes is not reproducible.
	set(NPM_STAMP "${CMAKE_CURRENT_BINARY_DIR}/npm_install.stamp")
	add_custom_command(
		OUTPUT "${NPM_STAMP}"
		COMMAND "${NPM_EXECUTABLE}" ci --prefer-offline --no-audit --no-fund
		COMMAND "${CMAKE_COMMAND}" -E touch "${NPM_STAMP}"
		WORKING_DIRECTORY "${WEB_SOURCE_DIR}"
		DEPENDS "${WEB_SOURCE_DIR}/package.json" "${WEB_SOURCE_DIR}/package-lock.json"
		COMMENT "Installing web dependencies (npm ci)"
		VERBATIM
	)

	# Built into the BUILD tree, not the source tree: a build directory is disposable and
	# should not leave artefacts behind in Web/.
	add_custom_command(
		OUTPUT "${WEB_DIST_DIR}/index.html"
		COMMAND "${NPM_EXECUTABLE}" run build -- --outDir "${WEB_DIST_DIR}" --emptyOutDir
		WORKING_DIRECTORY "${WEB_SOURCE_DIR}"
		DEPENDS "${NPM_STAMP}" ${WEB_SOURCES}
		COMMENT "Building the web UI (vite build)"
		VERBATIM
	)

	add_custom_command(
		OUTPUT "${WEB_GENERATED}"
		COMMAND "${CMAKE_COMMAND}"
			-DASSET_DIR=${WEB_DIST_DIR}
			-DOUTPUT=${WEB_GENERATED}
			-P "${EMBED_SCRIPT}"
		DEPENDS "${WEB_DIST_DIR}/index.html" "${EMBED_SCRIPT}"
		COMMENT "Embedding the web UI into the binary"
		VERBATIM
	)
else()
	# No npm, or deliberately disabled: an empty table, generated the same way so there is
	# only one code path for the consumer.
	add_custom_command(
		OUTPUT "${WEB_GENERATED}"
		COMMAND "${CMAKE_COMMAND}"
			-DASSET_DIR=
			-DOUTPUT=${WEB_GENERATED}
			-P "${EMBED_SCRIPT}"
		DEPENDS "${EMBED_SCRIPT}"
		COMMENT "Generating an empty web asset table (UI not built)"
		VERBATIM
	)
endif()

add_library(WebAssets STATIC "${WEB_GENERATED}")

target_include_directories(WebAssets
	PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(WebAssets PUBLIC cxx_std_20)
