#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

class Agent;

/* ---- Securities lending (OrderModelPlan Step 1.3) ----
*
* A short seller has to borrow the shares it sells, and the supply of shares to borrow is
* finite. That finiteness is what makes a squeeze possible: when lenders sell, shares leave
* the pool, loans are recalled, and borrowers are forced to buy back into a rising market.
*
* Supply comes from two places, as in reality:
*
*   institutional holders lend through agent-lender programmes. They earn the fee, less the
*   programme's cut. INST_LENDABLE_FRACTION of what they hold is in the pool.
*
*   retail MARGIN accounts' shares are rehypothecated by their broker. The customer earns
*   nothing; the fee is the broker's. RETAIL_LENDABLE_FRACTION of what they hold is in it.
*
* Loans are pooled through the broker, the way an agent-lender programme works, rather than
* matched lender to borrower. The fee is an annual rate set by utilisation (borrowed over
* lendable): flat at a general-collateral rate below a knee, rising steeply above it, which
* is the hard-to-borrow regime. The curve's shape is settled; its numbers are calibration
* register entries (D'Avolio 2002; Kolasinski, Reed & Ringgenberg 2013 are the candidates).
*/
inline constexpr double INST_LENDABLE_FRACTION = 0.50;
inline constexpr double RETAIL_LENDABLE_FRACTION = 0.30;
/* General-collateral fee, annual: what an easy-to-borrow stock costs */
inline constexpr double BORROW_GC_RATE = 0.003;
/* Utilisation above which the fee starts to climb */
inline constexpr double BORROW_KNEE = 0.60;
/* Annual fee as utilisation reaches 1 */
inline constexpr double BORROW_MAX_RATE = 0.30;
/* Share of an institutional lender's fee kept by the lending programme */
inline constexpr double BORROW_PROGRAMME_SHARE = 0.20;

class StockLoan {
public:
	/* Shares lenders have made available, institutional and retail, kept current per lender */
	double institutionalSupply = 0.0;
	double retailSupply = 0.0;
	/* Shares on loan to short sellers */
	unsigned long long borrowed = 0;
	/* Shares located for short sales that have not traded yet. Reserved, so two sellers
	*  cannot both be promised the last shares in the pool. */
	unsigned long long pending = 0;

	double supply() const { return this->institutionalSupply + this->retailSupply; }
	/* Shares that could be located right now */
	double available() const;
	/* Borrowed over lendable. Above 1, lenders have sold shares that are out on loan. */
	double utilisation() const;
	/* Annual borrow fee at a given utilisation */
	static double feeRate(double utilisation);

	/* What one agent puts in the pool, given what it holds */
	static double contributionOf(const Agent& agent);
	/* Re-read one agent's contribution, after its holdings or standing changed */
	void refreshLender(const Agent& agent);
	/* Rebuild every contribution from scratch, correcting any that drifted with the price */
	void rebuildSupply(const std::unordered_map<std::string, std::shared_ptr<Agent>>& agents);
	/* An institutional lender's cached contribution, 0 if it has none */
	double institutionalContribution(const std::string& agentId) const;

	/* Reserve shares for a short sale, false if the pool cannot cover them */
	bool locate(unsigned int shares);
	/* Release a located reservation that did not trade */
	void releaseLocate(unsigned int shares);
	/* A located short sale traded: the reservation becomes a loan */
	void openLoan(Agent& borrower, unsigned int shares, double nowMs);
	/* Borrow outright, for a market maker's short that traded unlocated. Returns how many
	*  shares the pool could actually lend, which may be fewer than asked. */
	unsigned int borrowUpTo(Agent& borrower, unsigned int shares, double nowMs);
	/* Shares returned when a short is covered */
	void returnLoan(Agent& borrower, unsigned int shares);

	/* Which loans to recall, newest first, to bring borrowed back within supply. Each entry
	*  is a borrower and how many of its shares are recalled. Changes nothing. */
	std::vector<std::pair<std::string, unsigned int>> recallsNeeded() const;

	void reset();

private:
	/* Each lender's cached contribution, so a change is O(1) rather than a rebuild */
	std::unordered_map<std::string, double> instContribution;
	std::unordered_map<std::string, double> retailContribution;
	/* When each borrower last borrowed, the order recalls work back through */
	std::unordered_map<std::string, double> lastBorrowMs;
	std::unordered_map<std::string, unsigned long long> loanOf;
};
