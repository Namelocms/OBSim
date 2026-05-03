cmake_minimum_required(VERSION 3.31)

project(OBSim)

include(FetchContent)

FetchContent_Declare(ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
  GIT_TAG v6.1.9
)

FetchContent_MakeAvailable(ftxui)
message(STATUS "FTXUI source dir: ${ftxui_SOURCE_DIR}")

add_subdirectory(Core)
add_subdirectory(App)
