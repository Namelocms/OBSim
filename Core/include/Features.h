#pragma once

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
};
