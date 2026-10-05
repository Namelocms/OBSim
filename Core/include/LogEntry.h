#pragma once
#include <string>

struct LogEntry {
	/* ALERT: market structure -- crosses, trading pauses, the short sale restriction.
	*  RISK: the broker protecting itself -- margin calls, write-offs, recalls, buy-ins.
	*  STOP: a held order triggering. All three added by OrderModelPlan Step 4.1. */
	enum class Kind { FILL, PLACE, CANCEL, HOLD, ALERT, RISK, STOP };
	Kind kind;
	double simTimeMs;
	std::string text;
};