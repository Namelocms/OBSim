#pragma once
#include "FeeSchedule.h"

/* ---- Feature switches ----
*
* One switch per mechanism the order model plan adds, so each can be measured on its own and
* the calibration phase gets ablations for free. Every switch defaults OFF, and the off path
* consumes no RNG and changes nothing, so a run with everything off reproduces the pre-plan
* market byte for byte (harnesses/sigdiff.ps1 is the proof, per step).
*
* Lives on OrderBook, the object agents, the broker and the matching engine all reach.
* CoreSim::run copies Config::features into it at the start of every run; a harness that
* drives the engine without run() sets OB.features directly.
*/
struct Features {
	/* OrderModelPlan Step 0.4: agents work their orders with cancel/replace -- an unwinding
	*  agent replaces its last attempt, an ALGO re-prices a quote the market has left behind
	*  -- instead of cancelling and re-posting, or sitting on a stale quote. */
	bool agentReplace = false;
	/* OrderModelPlan Step 0.5: directionalBias is the sign of the agent's net position (0 when
	*  flat) rather than +1 for everyone, and adversity is measured from where the position
	*  opened rather than from the level of sentiment. Changes transient departures. */
	bool adversityFromEntry = false;

	/* OrderModelPlan Step 1.1: what trading costs. Off, every trade is free, as it always was.
	*  On, each agent pays the schedule for its type. The defaults are the generic presets in
	*  FeeSchedule.h; any schedule can be swapped in. */
	struct Fees {
		bool enabled = false;
		FeeSchedule retail = FeeSchedule::zeroCommissionRetail();
		FeeSchedule institution = FeeSchedule::institutionalPerShare();
	} fees;

	/* OrderModelPlan Step 1.2: margin accounts. An account with at least MARGIN_MIN_EQUITY of
	*  equity may borrow against marginable stock under Reg T; every account with a debit is
	*  held to its maintenance requirement and liquidated the moment it falls below. Interest
	*  accrues daily. Off, every account is a cash account, as it always was. */
	struct Margin {
		bool enabled = false;
	} margin;

	/* OrderModelPlan Steps 1.3 and 1.4: securities lending and short selling. Requires margin,
	*  since Reg T only allows a short in a margin account; with margin off this does nothing. */
	struct Shorting {
		bool enabled = false;
	} shorting;

	/* OrderModelPlan Step 2.1: stop orders. The mechanism is always there -- it does nothing
	*  until someone places a stop -- but whether a stop may trigger outside the regular session
	*  is a broker policy. Most retail brokers only trigger stops in the regular session, so
	*  that is the default (decision D5); a print off hours is ignored, and a stop the price
	*  gapped through overnight fires on the first regular-session print. */
	struct Stops {
		bool extendedHours = false;
	} stops;
};
