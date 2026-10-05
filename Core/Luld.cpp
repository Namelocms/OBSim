#include "include/Luld.h"
#include "include/OrderBook.h"
#include "include/MarketCalendar.h"
#include "include/Util.h"
#include <algorithm>
#include <cmath>

namespace Luld {

double percentFor(double reference, int tier) {
	if (reference > 3.00) { return (tier == 1) ? 0.05 : 0.10; }
	if (reference >= 0.75) { return 0.20; }
	// The lesser of $0.15 or 75%, expressed as a fraction of the reference
	if (!(reference > 0.0)) { return 0.75; }
	return std::min(0.15 / reference, 0.75);
}

void computeBands(OrderBook& ob, double nowMs) {
	LuldState& l = ob.luld;
	double pct = percentFor(l.reference, ob.features.luld.tier);

	// The plan's doubling windows, measured from the day's regular open and close
	int day = MarketCalendar::dayIndex(nowMs);
	double open = MarketCalendar::sessionOpenMs(Session::REGULAR, day);
	double close = open + MarketCalendar::sessionLengthMs(Session::REGULAR);
	bool closingWindow = nowMs >= close - MarketCalendar::minutesToMs(LULD_CLOSE_DOUBLING_MINUTES);
	bool openingWindow = LULD_DOUBLE_AT_OPEN && nowMs < open + MarketCalendar::minutesToMs(LULD_OPEN_DOUBLING_MINUTES);
	bool doubledAtClose = (ob.features.luld.tier == 1) || l.reference <= 3.00;
	if ((closingWindow && doubledAtClose) || openingWindow) { pct *= 2.0; }

	double precision = (l.reference < 1.00) ? 0.0001 : 0.01;
	l.lower = std::max(roundTo(l.reference * (1.0 - pct), precision), precision);
	l.upper = roundTo(l.reference * (1.0 + pct), precision);
}

void start(OrderBook& ob, double reference, double nowMs) {
	LuldState& l = ob.luld;
	if (!ob.features.luld.enabled || !(reference > 0.0)) { l.active = false; return; }
	l.active = true;
	l.paused = false;
	l.reference = reference;
	l.window.clear();
	l.window.push_back({ nowMs, reference });
	l.limitStateSince = -1.0;
	computeBands(ob, nowMs);
}

void stop(OrderBook& ob) {
	ob.luld.active = false;
	ob.luld.paused = false;
	ob.luld.limitStateSince = -1.0;
	ob.luld.window.clear();
}

void onPrint(OrderBook& ob, double price, double nowMs) {
	LuldState& l = ob.luld;
	if (!l.active) { return; }

	l.window.push_back({ nowMs, price });
	double from = nowMs - MarketCalendar::minutesToMs(LULD_REFERENCE_WINDOW_MINUTES);
	while (!l.window.empty() && l.window.front().first < from) { l.window.pop_front(); }

	double sum = 0.0;
	for (const auto& p : l.window) { sum += p.second; }
	double mean = sum / double(l.window.size());

	// The reference only moves when the mean has moved 1% or more from it
	if (std::fabs(mean - l.reference) >= LULD_REFERENCE_MOVE * l.reference) {
		l.reference = mean;
		++l.bandUpdates;
	}
	// Recomputed on every print either way, so the closing doubling starts on time
	computeBands(ob, nowMs);
}

bool checkLimitState(OrderBook& ob, double nowMs) {
	LuldState& l = ob.luld;
	if (!l.active || l.paused) { return false; }

	std::vector<std::shared_ptr<Order>> bid = ob.peekBestN(OrderAction::BID, 1);
	std::vector<std::shared_ptr<Order>> ask = ob.peekBestN(OrderAction::ASK, 1);
	double tolerance = ob.tickPrecision * 0.5;
	bool atUpper = !bid.empty() && bid[0] != nullptr && bid[0]->price >= l.upper - tolerance;
	bool atLower = !ask.empty() && ask[0] != nullptr && ask[0]->price <= l.lower + tolerance;

	if (!atUpper && !atLower) { l.limitStateSince = -1.0; return false; }
	if (l.limitStateSince < 0.0) {
		l.limitStateSince = nowMs;
		++l.limitStates;
		return false;
	}
	if (nowMs - l.limitStateSince < LULD_LIMIT_STATE_SECONDS * 1000.0) { return false; }

	// 15 seconds at the band: a five minute pause, from when the 15 seconds ran out
	l.paused = true;
	l.pauseEndsMs = l.limitStateSince + LULD_LIMIT_STATE_SECONDS * 1000.0 + MarketCalendar::minutesToMs(LULD_PAUSE_MINUTES);
	l.limitStateSince = -1.0;
	++l.pauses;
	return true;
}

}
