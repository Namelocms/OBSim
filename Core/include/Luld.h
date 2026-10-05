#pragma once
#include <deque>
#include <utility>

class OrderBook;

/* ---- Limit Up-Limit Down (OrderModelPlan Step 3.2) ----
*
* Single-stock price bands, regular session only, as the LULD Plan sets them:
*
*   reference   the mean trade price over the preceding 5 minutes, moved only when the new
*               mean differs from the current reference by 1% or more; the opening price to
*               begin with
*   bands       Tier 1 / Tier 2:  5% / 10% above $3.00;  20% from $0.75 to $3.00;  the lesser
*               of $0.15 or 75% below $0.75
*   doubling    since Amendment 18 (2020-02-24), the 15:35-16:00 window doubles the bands for
*               Tier 1 and for Tier 2 at or under $3.00, and nothing is doubled at the open.
*               Checked 2026-10-04 against Nasdaq UTP Vendor Alert 2019-9; one secondary source
*               reads the opening differently, so it is a single switch (LULD_DOUBLE_AT_OPEN).
*
* No trade prints outside the bands. A book sitting at a band -- best bid at the upper band,
* or best offer at the lower -- is in a LIMIT STATE; one that stays there for 15 seconds goes
* into a 5 minute TRADING PAUSE, during which orders are accepted and cancelled but nothing
* matches, and which ends with a reopening cross.
*/
inline constexpr double LULD_REFERENCE_WINDOW_MINUTES = 5.0;
inline constexpr double LULD_REFERENCE_MOVE = 0.01;
inline constexpr double LULD_LIMIT_STATE_SECONDS = 15.0;
inline constexpr double LULD_PAUSE_MINUTES = 5.0;
/* Minutes before the regular close at which the closing doubling starts (15:35) */
inline constexpr double LULD_CLOSE_DOUBLING_MINUTES = 25.0;
/* Amendment 18 removed the 09:30-09:45 doubling. Set true to restore it. */
inline constexpr bool LULD_DOUBLE_AT_OPEN = false;
inline constexpr double LULD_OPEN_DOUBLING_MINUTES = 15.0;

struct LuldState {
	/* Bands in force: the regular session, with LULD switched on, and not paused */
	bool active = false;
	double reference = 0.0;
	double lower = 0.0;
	double upper = 0.0;
	/* Prints inside the reference window, as (sim time, price) */
	std::deque<std::pair<double, double>> window;
	/* When the current limit state began, or < 0 for none */
	double limitStateSince = -1.0;
	/* A trading pause, and when it ends */
	bool paused = false;
	double pauseEndsMs = 0.0;

	// ---- Measurement only ----
	long long limitStates = 0;
	long long pauses = 0;
	long long bandUpdates = 0;

	void reset() { *this = LuldState(); }
};

namespace Luld {

/* The band's percentage for a reference price and tier, before any doubling */
double percentFor(double reference, int tier);
/* Recompute the bands from the reference, doubling them in the closing window where the plan does */
void computeBands(OrderBook& ob, double nowMs);
/* Begin banding at the open (or a reopening) from this reference */
void start(OrderBook& ob, double reference, double nowMs);
/* Stop banding at the close */
void stop(OrderBook& ob);
/* A trade printed: roll the reference window, and move the reference if it is 1% stale */
void onPrint(OrderBook& ob, double price, double nowMs);
/* Look at the touch against the bands. Enters or leaves a limit state, and returns true if a
*  limit state has now lasted LULD_LIMIT_STATE_SECONDS, starting a trading pause. */
bool checkLimitState(OrderBook& ob, double nowMs);

}
