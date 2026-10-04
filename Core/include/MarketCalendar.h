#pragma once
#include <cmath>
#include "Enums.h"

/* ---- Market Calendar ----
*
* Maps simulation time onto US equity market sessions.
*
* t = 0 is 04:00 (PREMARKET open) on day 0. Sim time is a monotonic millisecond
* counter that runs continuously through back-data initialization into the live
* simulation, so timestamps and the chart x-axis never break at the handoff.
*
* A full day is 1440 minutes of clock time, of which 960 are tradeable sessions
* and 480 are overnight. The overnight span is always accounted for in the clock
* (whether or not it is simulated) so that time-of-day stays aligned across days.
*
*   SESSION______||  CLOCK__________||  OFFSET___||  LENGTH
*   PREMARKET____||  04:00 - 09:30__||  0________||  330 min
*   REGULAR______||  09:30 - 16:00__||  330______||  390 min
*   AFTERHOURS___||  16:00 - 20:00__||  720______||  240 min
*   OVERNIGHT____||  20:00 - 04:00__||  960______||  480 min
*/
namespace MarketCalendar {

// ---- Session Lengths (minutes) ----

inline constexpr double PREMARKET_MINUTES = 330.0;
inline constexpr double REGULAR_MINUTES = 390.0;
inline constexpr double AFTERHOURS_MINUTES = 240.0;
inline constexpr double OVERNIGHT_MINUTES = 480.0;

/* Tradeable minutes in a day, excludes overnight */
inline constexpr double ACTIVE_MINUTES_PER_DAY = PREMARKET_MINUTES + REGULAR_MINUTES + AFTERHOURS_MINUTES; // 960
/* Total clock minutes in a day, includes overnight */
inline constexpr double TOTAL_MINUTES_PER_DAY = ACTIVE_MINUTES_PER_DAY + OVERNIGHT_MINUTES; // 1440

// ---- Session Offsets From Day Start (minutes) ----

inline constexpr double PREMARKET_OPEN_MINUTES = 0.0;
inline constexpr double REGULAR_OPEN_MINUTES = PREMARKET_OPEN_MINUTES + PREMARKET_MINUTES;   // 330
inline constexpr double AFTERHOURS_OPEN_MINUTES = REGULAR_OPEN_MINUTES + REGULAR_MINUTES;    // 720
inline constexpr double OVERNIGHT_OPEN_MINUTES = AFTERHOURS_OPEN_MINUTES + AFTERHOURS_MINUTES; // 960

inline constexpr double MS_PER_MINUTE = 60'000.0;

// ---- Conversions ----

/* Convert minutes to milliseconds of sim time */
inline constexpr double minutesToMs(double minutes) { return minutes * MS_PER_MINUTE; }
/* Convert milliseconds of sim time to minutes */
inline constexpr double msToMinutes(double ms) { return ms / MS_PER_MINUTE; }

// ---- Wall Clock Mapping ----

/* Unix time of sim time 0, i.e. 04:00 UTC on Monday 1 January 2024
*
* Sim time is an offset in milliseconds and carries no date of its own. A chart with a
* real time axis needs one, so this pins t = 0 to a concrete instant. Monday is chosen so
* day 0 is a weekday, which is what a market day claims to be.
*/
inline constexpr double SIM_EPOCH_BASE_SEC = 1'704'081'600.0;

/* Map sim time onto a Unix timestamp in seconds
*
* Exact, not approximate: a sim day is TOTAL_MINUTES_PER_DAY = 1440 minutes, so it is a
* real 24 hour day and the mapping is a straight offset. t = 0 lands on 04:00, the
* premarket open, and every session boundary lands on the wall clock time it is named for.
*
* Returns fractional seconds. Sim time resolves to milliseconds and finer, so rounding
* here would quietly collapse trades that a caller may still want to tell apart. Whoever
* is drawing decides the resolution -- floor to the bar interval -- rather than having it
* decided for them.
*
* LOSSY below about a quarter of a microsecond, and unavoidably so. The base is ~1.7e9
* seconds, where one ULP of a double is 2^-22 s (~0.00024 ms), so adding it spends most
* of the mantissa on the date. Round tripping through epochSecToSimTime is therefore
* accurate to ~0.0005 ms, not exact. That is three orders of magnitude finer than the
* fastest agent cadence in the sim (institutional ALGO, 0.1-1 ms), so no two events can
* collide -- but it means this value is for DRAWING. Anything that needs the real time of
* a trade should carry simTimeMs itself rather than converting back.
*
* Two properties a caller should know before trusting the axis. Sim days run back to back
* with no weekends or holidays, so consecutive days map to consecutive calendar days
* including Saturdays. And a skipped overnight is a real gap in the series, not missing
* data: nothing traded there because the window was never simulated.
*/
inline constexpr double simTimeToEpochSec(double simTimeMs) {
	return SIM_EPOCH_BASE_SEC + (simTimeMs / 1000.0);
}
/* Map a Unix timestamp in seconds back to sim time, the inverse of simTimeToEpochSec
*
* For turning a time a viewer picked on the chart back into something the engine can be
* asked about. Can return a negative value if given an instant before the run began.
*/
inline constexpr double epochSecToSimTime(double epochSec) {
	return (epochSec - SIM_EPOCH_BASE_SEC) * 1000.0;
}

// ---- Day Operations ----

/* Get the zero-based day the given sim time falls on */
inline int dayIndex(double simTimeMs) {
	return int(std::floor(msToMinutes(simTimeMs) / TOTAL_MINUTES_PER_DAY));
}
/* Get the minutes elapsed since the start (04:00) of the day the given sim time falls on */
inline double minutesIntoDay(double simTimeMs) {
	double into = std::fmod(msToMinutes(simTimeMs), TOTAL_MINUTES_PER_DAY);
	return (into < 0.0) ? into + TOTAL_MINUTES_PER_DAY : into;
}
/* Get the sim time at the start (04:00) of the given day */
inline double dayStartMs(int dayIndex) {
	return minutesToMs(dayIndex * TOTAL_MINUTES_PER_DAY);
}
/* Get the tradeable minutes elapsed since t = 0, overnight spans excluded
*
* Total elapsed minutes include overnight, this counts only session time. For a
* 1-day back data run ending at the regular open: 1770 total, 1290 active.
*/
inline double activeMinutesElapsed(double simTimeMs) {
	double into = minutesIntoDay(simTimeMs);
	double activeIntoDay = (into < ACTIVE_MINUTES_PER_DAY) ? into : ACTIVE_MINUTES_PER_DAY;
	return (dayIndex(simTimeMs) * ACTIVE_MINUTES_PER_DAY) + activeIntoDay;
}

// ---- Session Operations ----

/* Get the market session the given sim time falls in */
inline Session sessionAt(double simTimeMs) {
	double into = minutesIntoDay(simTimeMs);

	if (into < REGULAR_OPEN_MINUTES) { return Session::PREMARKET; }        // [0, 330)
	if (into < AFTERHOURS_OPEN_MINUTES) { return Session::REGULAR; }       // [330, 720)
	if (into < OVERNIGHT_OPEN_MINUTES) { return Session::AFTERHOURS; }     // [720, 960)
	return Session::OVERNIGHT;                                             // [960, 1440)
}
/* Get a session's offset from the start (04:00) of its day */
inline double sessionOffsetMs(Session session) {
	switch (session) {
	case Session::PREMARKET:  return minutesToMs(PREMARKET_OPEN_MINUTES);
	case Session::REGULAR:    return minutesToMs(REGULAR_OPEN_MINUTES);
	case Session::AFTERHOURS: return minutesToMs(AFTERHOURS_OPEN_MINUTES);
	case Session::OVERNIGHT:  return minutesToMs(OVERNIGHT_OPEN_MINUTES);
	default:                  return minutesToMs(PREMARKET_OPEN_MINUTES); // CLOSED, reserved for holidays/weekends
	}
}
/* Get the duration of a session */
inline double sessionLengthMs(Session session) {
	switch (session) {
	case Session::PREMARKET:  return minutesToMs(PREMARKET_MINUTES);
	case Session::REGULAR:    return minutesToMs(REGULAR_MINUTES);
	case Session::AFTERHOURS: return minutesToMs(AFTERHOURS_MINUTES);
	case Session::OVERNIGHT:  return minutesToMs(OVERNIGHT_MINUTES);
	default:                  return 0.0; // CLOSED, reserved for holidays/weekends
	}
}
/* Get the sim time a session opens on the given day
*
* Doubles as the back-data duration: the live sim begins at the open of the
* selected session, backDataDays after t = 0.
* sessionOpenMs(REGULAR, 1) = 1 full day (1440) + premarket (330) = 1770 minutes
*/
inline double sessionOpenMs(Session session, int dayIndex) {
	return dayStartMs(dayIndex) + sessionOffsetMs(session);
}
/* Get the sim time the session containing the given sim time ends
*
* The end of OVERNIGHT is the start (04:00) of the following day.
*/
inline double sessionEndMs(double simTimeMs) {
	Session session = sessionAt(simTimeMs);
	return dayStartMs(dayIndex(simTimeMs)) + sessionOffsetMs(session) + sessionLengthMs(session);
}
/* Get the sim time of the next session change, equivalent to the end of the current session */
inline double nextBoundaryMs(double simTimeMs) {
	return sessionEndMs(simTimeMs);
}
/* Check whether a session is tradeable */
inline bool isActiveSession(Session session) {
	return session == Session::PREMARKET || session == Session::REGULAR || session == Session::AFTERHOURS;
}

// ---- Participation Curve Operations ----

/* Progress through the post-close decay span, 0 at the afterhours open, 1 at the next premarket open
*
* Afterhours and overnight form one continuous decay. Activity tapers from the
* close, keeps falling through the night, and bottoms out just before premarket.
* Returns 0 for any time before the afterhours open.
*/
inline double closingDecayProgress(double simTimeMs) {
	double into = minutesIntoDay(simTimeMs);
	if (into < AFTERHOURS_OPEN_MINUTES) { return 0.0; }
	return (into - AFTERHOURS_OPEN_MINUTES) / (AFTERHOURS_MINUTES + OVERNIGHT_MINUTES);
}
/* Progress through premarket, 0 at the premarket open, 1 at the regular open
*
* Mirrors the closing decay: participation builds back up through premarket and
* is whole again by the opening bell.
*/
inline double premarketProgress(double simTimeMs) {
	double into = minutesIntoDay(simTimeMs);
	if (into >= REGULAR_OPEN_MINUTES) { return 1.0; }
	return into / PREMARKET_MINUTES;
}

// ---- Order Expiry Operations ----

/* Check whether resting orders expire when the given session closes
*
* PREMARKET is NOT an expiring boundary. It rolls straight into REGULAR the way
* it does in real markets, so a day order placed premarket stays live through
* the regular session.
*/
inline bool expiresAtSessionEnd(Session session) {
	return session == Session::REGULAR || session == Session::AFTERHOURS || session == Session::OVERNIGHT;
}
/* Get the sim time of the next boundary that expires resting orders
*
* This is the end of the current session, except from PREMARKET, which rolls
* into REGULAR and so expires at the regular close instead.
* Chaining is safe: passing a returned boundary back in yields the following one.
*/
inline double nextExpiryBoundaryMs(double simTimeMs) {
	Session session = sessionAt(simTimeMs);
	if (expiresAtSessionEnd(session)) { return sessionEndMs(simTimeMs); }

	// PREMARKET (and reserved CLOSED) roll forward to the regular close
	return dayStartMs(dayIndex(simTimeMs)) + sessionOffsetMs(Session::REGULAR) + sessionLengthMs(Session::REGULAR);
}
/* Get the sim time a DAY order placed at simTimeMs expires, given the sessions it may trade in
*
* The close of the LAST session in the mask on the trading day the order belongs to: the
* regular close for a regular-session order, the afterhours close for an extended-hours
* one, the following 04:00 for one that may trade overnight. A sim day runs from its 04:00
* premarket open through the end of its overnight, so "the day" is dayIndex(simTimeMs).
*
* If that close has already passed -- a regular-session order placed afterhours -- the
* order belongs to the next trading day and lives to that day's close, as a broker treats
* a day order entered after the session it is for.
*
* Every value returned is the end of a session, so it coincides with a boundary the
* engine already processes. Returns simTimeMs itself for an empty mask, which no valid
* order has.
*/
inline double dayOrderExpiryMs(double simTimeMs, SessionMask sessions) {
	const Session latestFirst[] = { Session::OVERNIGHT, Session::AFTERHOURS, Session::REGULAR, Session::PREMARKET };

	for (int day = dayIndex(simTimeMs); day <= dayIndex(simTimeMs) + 1; ++day) {
		for (Session session : latestFirst) {
			if ((sessions & sessionBit(session)) == 0) { continue; }
			double closeMs = sessionOpenMs(session, day) + sessionLengthMs(session);
			if (closeMs > simTimeMs) { return closeMs; }
			break; // the latest eligible session of this day is already over, try the next day
		}
	}
	return simTimeMs;
}

}
