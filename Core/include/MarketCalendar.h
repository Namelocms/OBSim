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

}
