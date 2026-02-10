add_executable(OBSim_ConsoleApp
	"MVP_Benchmark_main.cpp"
 "main.cpp")
target_link_libraries(OBSim_ConsoleApp
	PRIVATE
		Simulator
)