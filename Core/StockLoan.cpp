#include "include/StockLoan.h"
#include "include/Agent.h"
#include "include/Account.h"
#include "include/Enums.h"
#include <algorithm>
#include <cmath>
#include <limits>

double StockLoan::available() const {
	double free = this->supply() - double(this->borrowed) - double(this->pending);
	return (free > 0.0) ? free : 0.0;
}

double StockLoan::utilisation() const {
	double s = this->supply();
	if (s > 0.0) { return double(this->borrowed) / s; }
	return (this->borrowed > 0) ? std::numeric_limits<double>::infinity() : 0.0;
}

double StockLoan::feeRate(double utilisation) {
	if (!(utilisation > BORROW_KNEE)) { return BORROW_GC_RATE; }
	double over = std::min((utilisation - BORROW_KNEE) / (1.0 - BORROW_KNEE), 1.0);
	return BORROW_GC_RATE + (BORROW_MAX_RATE - BORROW_GC_RATE) * over * over;
}

double StockLoan::contributionOf(const Agent& agent) {
	// The user's shares are not lent: the daily rebuild walks the population, which the user
	// is not part of, so a contribution would come and go with each trade
	if (agent.isUser) { return 0.0; }
	double held = double(Account::longShares(agent));
	if (held <= 0.0) { return 0.0; }
	if (agent.type == AgentType::INSTITUTION) { return held * INST_LENDABLE_FRACTION; }
	// Only a margin account's shares can be rehypothecated
	return Account::hasMarginPrivileges(agent) ? held * RETAIL_LENDABLE_FRACTION : 0.0;
}

void StockLoan::refreshLender(const Agent& agent) {
	double c = contributionOf(agent);
	auto& map = (agent.type == AgentType::INSTITUTION) ? this->instContribution : this->retailContribution;
	double& total = (agent.type == AgentType::INSTITUTION) ? this->institutionalSupply : this->retailSupply;

	auto found = map.find(agent.id);
	double old = (found == map.end()) ? 0.0 : found->second;
	total += c - old;
	if (total < 0.0) { total = 0.0; }   // float noise from many small updates
	if (c > 0.0) { map[agent.id] = c; }
	else if (found != map.end()) { map.erase(found); }
}

void StockLoan::rebuildSupply(const std::unordered_map<std::string, std::shared_ptr<Agent>>& agents) {
	this->instContribution.clear();
	this->retailContribution.clear();
	this->institutionalSupply = 0.0;
	this->retailSupply = 0.0;
	for (const auto& kv : agents) {
		if (kv.second == nullptr) { continue; }
		double c = contributionOf(*kv.second);
		if (c <= 0.0) { continue; }
		if (kv.second->type == AgentType::INSTITUTION) {
			this->instContribution[kv.first] = c;
			this->institutionalSupply += c;
		}
		else {
			this->retailContribution[kv.first] = c;
			this->retailSupply += c;
		}
	}
}

double StockLoan::institutionalContribution(const std::string& agentId) const {
	auto found = this->instContribution.find(agentId);
	return (found == this->instContribution.end()) ? 0.0 : found->second;
}

bool StockLoan::locate(unsigned int shares) {
	if (shares == 0 || double(shares) > this->available()) { return false; }
	this->pending += shares;
	return true;
}

void StockLoan::releaseLocate(unsigned int shares) {
	this->pending -= std::min<unsigned long long>(this->pending, shares);
}

void StockLoan::openLoan(Agent& borrower, unsigned int shares, double nowMs) {
	if (shares == 0) { return; }
	this->releaseLocate(shares);
	this->borrowed += shares;
	borrower.borrowedShares += shares;
	this->loanOf[borrower.id] += shares;
	this->lastBorrowMs[borrower.id] = nowMs;
}

unsigned int StockLoan::borrowUpTo(Agent& borrower, unsigned int shares, double nowMs) {
	unsigned int can = (unsigned int)std::min<double>(double(shares), std::floor(this->available()));
	if (can == 0) { return 0; }
	this->borrowed += can;
	borrower.borrowedShares += can;
	this->loanOf[borrower.id] += can;
	this->lastBorrowMs[borrower.id] = nowMs;
	return can;
}

void StockLoan::returnLoan(Agent& borrower, unsigned int shares) {
	unsigned int back = std::min(shares, borrower.borrowedShares);
	if (back == 0) { return; }
	borrower.borrowedShares -= back;
	this->borrowed -= std::min<unsigned long long>(this->borrowed, back);
	auto found = this->loanOf.find(borrower.id);
	if (found != this->loanOf.end()) {
		found->second -= std::min<unsigned long long>(found->second, back);
		if (found->second == 0) { this->loanOf.erase(found); this->lastBorrowMs.erase(borrower.id); }
	}
}

std::vector<std::pair<std::string, unsigned int>> StockLoan::recallsNeeded() const {
	std::vector<std::pair<std::string, unsigned int>> recalls;
	double s = std::floor(this->supply());
	if (double(this->borrowed) <= s) { return recalls; }
	unsigned long long excess = this->borrowed - (unsigned long long)s;

	// Newest loans first. Ties on time break on id, so the order is deterministic.
	std::vector<std::pair<double, std::string>> byAge;
	byAge.reserve(this->loanOf.size());
	for (const auto& kv : this->loanOf) {
		auto t = this->lastBorrowMs.find(kv.first);
		byAge.push_back({ (t == this->lastBorrowMs.end()) ? 0.0 : t->second, kv.first });
	}
	std::sort(byAge.begin(), byAge.end(), [](const auto& a, const auto& b) {
		if (a.first != b.first) { return a.first > b.first; }
		return a.second < b.second;
	});

	for (const auto& entry : byAge) {
		if (excess == 0) { break; }
		unsigned long long loan = this->loanOf.at(entry.second);
		unsigned long long take = std::min(loan, excess);
		recalls.push_back({ entry.second, (unsigned int)take });
		excess -= take;
	}
	return recalls;
}

void StockLoan::reset() {
	*this = StockLoan();
}
