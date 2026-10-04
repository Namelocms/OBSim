#include "include/TriggerBook.h"
#include <algorithm>

void TriggerBook::setMargin(const std::string& agentId, Side side, double price) {
	this->clearMargin(agentId);
	if (!(price > 0.0)) { return; }

	TriggerEntry entry{ TriggerKind::MARGIN, agentId, "", false, this->nextSeq++ };
	MarginSlot slot{ side, this->sells.end(), this->buys.end() };
	if (side == Side::SELL) { slot.sell = this->sells.emplace(price, entry); }
	else { slot.buy = this->buys.emplace(price, entry); }
	this->margin[agentId] = slot;
}

void TriggerBook::clearMargin(const std::string& agentId) {
	auto found = this->margin.find(agentId);
	if (found == this->margin.end()) { return; }
	if (found->second.side == Side::SELL) { this->sells.erase(found->second.sell); }
	else { this->buys.erase(found->second.buy); }
	this->margin.erase(found);
}

bool TriggerBook::marginPrice(const std::string& agentId, double& price) const {
	auto found = this->margin.find(agentId);
	if (found == this->margin.end()) { return false; }
	price = (found->second.side == Side::SELL) ? found->second.sell->first : found->second.buy->first;
	return true;
}

std::vector<TriggerEntry> TriggerBook::collect(double low, double high) {
	std::vector<TriggerEntry> fired;

	// A sell trigger at T fires on a print at or below T (strictly below if not inclusive).
	// The lowest print in the range is the one that reached furthest down.
	for (auto it = this->sells.begin(); it != this->sells.end() && it->first >= low;) {
		bool crossed = it->second.inclusive ? (low <= it->first) : (low < it->first);
		if (!crossed) { ++it; continue; }
		fired.push_back(it->second);
		if (it->second.kind == TriggerKind::MARGIN) { this->margin.erase(it->second.agentId); }
		it = this->sells.erase(it);
	}

	// A buy trigger at T fires on a print at or above T
	for (auto it = this->buys.begin(); it != this->buys.end() && it->first <= high;) {
		bool crossed = it->second.inclusive ? (high >= it->first) : (high > it->first);
		if (!crossed) { ++it; continue; }
		fired.push_back(it->second);
		if (it->second.kind == TriggerKind::MARGIN) { this->margin.erase(it->second.agentId); }
		it = this->buys.erase(it);
	}

	return fired;
}

void TriggerBook::clear() {
	this->sells.clear();
	this->buys.clear();
	this->margin.clear();
	this->nextSeq = 1;
}
