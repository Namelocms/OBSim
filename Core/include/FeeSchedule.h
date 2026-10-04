#pragma once
#include <algorithm>

/* ---- Fee schedule ----
*
* Everything a trade costs beyond its price, as plain data (OrderModelPlan Step 1.1, #2).
* Plain data on purpose: a broker preset dropdown later is a list of these, so adding a
* broker is a data change, not a code change. The engine has no broker names in it; the two
* presets below are generic shapes built from published rules and typical pricing.
*
* Three payees, charged per executed leg in MatchingEngine::settleLeg:
*
*   broker      commission, per share and/or per order, with a minimum and a cap
*   exchange    maker-taker: the side that takes liquidity pays a fee, the side that rested
*               earns a rebate. Either may be negative, which is how an inverted venue is
*               expressed. A zero-commission retail broker does not pass these through.
*   regulators  on SALES only: SEC Section 31 per dollar sold, FINRA TAF per share sold with
*               a per-trade cap
*
* Commission, the TAF cap and rounding work at the ORDER level, across however many legs an
* order fills in, because that is how they are defined: a commission minimum applies to the
* order, not to each fill. See MatchingEngine::chargeFees.
*/
struct FeeSchedule {
	/* Shown in a preset list, and in logs */
	const char* name = "none";
	/* Date the published figures below were checked. Fee schedules go stale. */
	const char* asOf = "";

	// ---- Broker commission ----
	double commissionPerShare = 0.0;
	double commissionPerOrder = 0.0;
	/* Least an order with any fill is charged */
	double commissionMin = 0.0;
	/* Most an order is charged, as a fraction of its traded value. 0 means uncapped. */
	double commissionMaxPct = 0.0;

	// ---- Exchange maker-taker ----
	/* False for a zero-commission retail broker: its customers never see exchange fees */
	bool passThroughExchangeFees = false;
	/* Per share, for trades at or above $1 */
	double takerFeePerShare = 0.0;
	/* Per share, for trades at or above $1. Positive is paid TO the agent. */
	double makerRebatePerShare = 0.0;
	/* Below $1 the access-fee cap is a share of the price rather than a flat amount */
	double subDollarTakerFeePct = 0.0;
	double subDollarMakerRebatePct = 0.0;

	// ---- Regulatory, on sales only ----
	/* SEC Section 31, per dollar sold */
	double secFeeRate = 0.0;
	/* FINRA Trading Activity Fee, per share sold */
	double tafPerShare = 0.0;
	/* FINRA TAF cap per trade */
	double tafMaxPerTrade = 0.0;

	/* Commission on an order that has filled `shares` for `value` in total so far */
	double commission(unsigned int shares, double value) const {
		if (shares == 0) { return 0.0; }
		double c = this->commissionPerShare * shares + this->commissionPerOrder;
		c = (std::max)(c, this->commissionMin);
		if (this->commissionMaxPct > 0.0) { c = (std::min)(c, this->commissionMaxPct * value); }
		return c;
	}
	/* Exchange fee per share for taking liquidity at this price, 0 if not passed through */
	double takerFee(double price) const {
		if (!this->passThroughExchangeFees) { return 0.0; }
		return (price >= 1.0) ? this->takerFeePerShare : this->subDollarTakerFeePct * price;
	}
	/* Exchange rebate per share for providing liquidity at this price, 0 if not passed through */
	double makerRebate(double price) const {
		if (!this->passThroughExchangeFees) { return 0.0; }
		return (price >= 1.0) ? this->makerRebatePerShare : this->subDollarMakerRebatePct * price;
	}
	/* TAF on an order that has sold `shares` in total so far */
	double taf(unsigned int shares) const {
		double t = this->tafPerShare * shares;
		return (this->tafMaxPerTrade > 0.0) ? (std::min)(t, this->tafMaxPerTrade) : t;
	}
	/* The most buying `shares` at a limit of `price` can cost in fees, for a bid's escrow
	*
	* Commission at the full size, and the taker fee as if every share took liquidity at the
	* limit. A buy pays no regulatory fees. Rebates only lower it, so they are ignored.
	*/
	double worstCaseBuyFees(unsigned int shares, double price) const {
		return this->commission(shares, shares * price) + this->takerFee(price) * shares;
	}
	/* Per-share and fixed parts of what buying costs in fees, for sizing an order
	*
	* Deliberately an over-estimate: the commission minimum and per-order charge are both
	* counted as fixed, and the cap is ignored, so an order sized against this can always
	* carry its worst-case fees.
	*/
	double buyFeePerShare(double price) const { return this->commissionPerShare + this->takerFee(price); }
	double buyFeeFixed() const { return this->commissionPerOrder + this->commissionMin; }

	/* ---- Presets ----
	*
	* Regulatory rates checked 2026-10-04:
	*   SEC Section 31  $20.60 per million dollars sold, from 2026-04-04 (SEC Fee Rate
	*                   Advisory 2026-2), in effect until 60 days after the FY2027 appropriation
	*   FINRA TAF       $0.000195 per share sold, max $9.79 per trade, the 2026 rate (FINRA fee
	*                   adjustment schedule, SR-FINRA-2024-019). FINRA has paused it at $0.00
	*                   for 2026-10-01 to 2026-12-31 (SR-FINRA-2026-021); the standing rate is
	*                   used, since a market model should not carry a three month holiday.
	*                   2027's scheduled rate is $0.000232, max $11.61.
	*   Access fee cap  Reg NMS Rule 610: $0.003 per share at or above $1, 0.3% of the price
	*                   below. The SEC's 2024 amendments lower it to $0.001 and 0.1%, but
	*                   compliance is deferred to November 2027, so the old cap still binds.
	*/

	/* A zero-commission retail broker: no commission, no exchange fees passed through,
	*  regulatory fees on sales passed through as they are at real retail brokers. */
	static FeeSchedule zeroCommissionRetail() {
		FeeSchedule f;
		f.name = "Zero-commission retail";
		f.asOf = "2026-10-04";
		f.secFeeRate = 20.60 / 1'000'000.0;
		f.tafPerShare = 0.000195;
		f.tafMaxPerTrade = 9.79;
		return f;
	}
	/* Institutional low-touch, per-share pricing with exchange fees passed through
	*
	* Commission $0.0035 a share, minimum $0.35, capped at 1% of trade value, which is the
	* shape of published per-share professional pricing. Exchange fees at the access-fee cap
	* for takers and a typical $0.0020 rebate for makers above $1; no sub-dollar rebate.
	*/
	static FeeSchedule institutionalPerShare() {
		FeeSchedule f = zeroCommissionRetail();
		f.name = "Institutional per-share";
		f.commissionPerShare = 0.0035;
		f.commissionMin = 0.35;
		f.commissionMaxPct = 0.01;
		f.passThroughExchangeFees = true;
		f.takerFeePerShare = 0.0030;
		f.makerRebatePerShare = 0.0020;
		f.subDollarTakerFeePct = 0.003;
		f.subDollarMakerRebatePct = 0.0;
		return f;
	}
};
