add_executable(OBSim_ConsoleApp
    "MVP_Benchmark_main.cpp"
    "main.cpp"
    
    "include/SimTUI.h"
 "SimTUI.cpp")
target_link_libraries(OBSim_ConsoleApp
    PRIVATE
        Simulator
        ftxui::component
        ftxui::dom
        ftxui::screen
)
target_include_directories(OBSim_ConsoleApp
    PRIVATE
        ${ftxui_SOURCE_DIR}/include
)