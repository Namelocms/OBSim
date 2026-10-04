#pragma once

/* ---- Ledger ----
*
* Every dollar that enters, leaves or moves outside of a trade, named.
*
* A trade moves cash between two agents and conserves it exactly (to the cent per leg). The
* things this records do not: endowments create money, a pooled transient's leftover cash
* leaves with it, and -- from OrderModelPlan Phase 1 on -- fees, interest and write-offs
* move money from agents to the house. Recording them makes cash conservation an exact,
* checkable statement instead of something that only holds with transients switched off:
*
*     sum(cash + escrowedCash) over agents  =  minted - retired - houseTotal()
*
* Lives on OrderBook because that is the one object the matching engine, the broker and the
* agents all already reach. Recording never consumes RNG and never feeds back into a
* decision, so it cannot change what the market does.
*/
struct Ledger {
	/* Cash created: resident endowments at construction, transient endowments on arrival */
	double minted = 0.0;
	/* Cash removed: a transient slot's leftover balance when it is rerolled for a new occupant */
	double retired = 0.0;

	// ---- House accounts, all zero until OrderModelPlan Phase 1 ----

	/* Broker commissions charged */
	double commissions = 0.0;
	/* Exchange taker fees charged, net of maker rebates paid */
	double exchangeFees = 0.0;
	/* SEC Section 31 and FINRA TAF on sales */
	double regulatoryFees = 0.0;
	/* Interest charged on margin debit balances */
	double marginInterest = 0.0;
	/* The broker's share of stock borrow fees (the lenders' share is paid to agents) */
	double borrowFees = 0.0;
	/* Negative equity written off after a liquidation gapped through, as a positive amount */
	double brokerLosses = 0.0;

	/* Net money the house has taken out of agents' hands. A write-off is money the house
	*  LOST, so it counts against: agents ended up holding that much more than they paid for. */
	double houseTotal() const {
		return this->commissions + this->exchangeFees + this->regulatoryFees
			+ this->marginInterest + this->borrowFees - this->brokerLosses;
	}

	void reset() { *this = Ledger(); }
};
