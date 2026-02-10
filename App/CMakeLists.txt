add_executable(OBSim_ConsoleApp
	main.cpp
)
target_link_libraries(OBSim_ConsoleApp
	PRIVATE
		Simulator
)