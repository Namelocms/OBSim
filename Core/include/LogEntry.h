#pragma once
#include <string>

struct LogEntry {
	enum class Kind { FILL, PLACE, CANCEL, HOLD };
	Kind kind;
	double simTimeMs;
	std::string text;
};