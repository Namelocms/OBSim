#include "include/Broker.h"
#include "include/Account.h"
#include "include/Agent.h"
#include "include/Order.h"
#include "include/Holding.h"
#include "include/OrderBook.h"
#include "include/MatchingEngine.h"
#include "include/SimClock.h"
#include "include/Enums.h"
#include "include/Util.h"
#include "include/MarketCalendar.h"
#include "include/FeeSchedule.h"
#include <cmath>
#include <unordered_set>
#include "include/Ledger.h"

Broker::Broker(OrderBook& ob, MatchingEngine& me) : OB(ob), ME(me) {}

std::shared_ptr<Order> Broker::submit(const OrderRequest& request, const std::shared_ptr<Agent>& agent) {
	if (agent == nullptr || request.empty()) { return nullptr; }
	if (request.side != OrderAction::BID && request.side != OrderAction::ASK) { return nullptr; }

	// Measurement only, see BrokerStats. Read straight off the front of each set: a stale
	// cancelled order sitting there skews a sample slightly and changes nothing.
	if (!this->OB.bidQueue.empty() && !this->OB.askQueue.empty()) {
		this->stats.spreadSum += (*this->OB.askQueue.begin())->price - (*this->OB.bidQueue.begin())->price;
		++this->stats.spreadSamples;
	}
	std::shared_ptr<Order> order = this->place(request, agent);
	if (order == nullptr) { ++this->stats.refused; }
	else { ++this->stats.submitted; }
	return order;
}

std::shared_ptr<Order> Broker::place(const OrderRequest& request, const std::shared_ptr<Agent>& agent) {

	const bool isLimit = (request.type == OrderType::LIMIT);
	if (isLimit && !(request.price > 0.0)) { return nullptr; }
	if ((request.sessions & SESSIONS_ALL) == 0) { return nullptr; }

	// ---- Time in force ----
	//
	// A market order never rests, so it is IOC whatever was asked -- a DAY or GTC market
	// order loses nothing by that. FOK is refused rather than coerced: silently turning
	// "all of it or none" into IOC would hand back a partial fill nobody agreed to.
	const double nowMs = this->OB.clock->simTimeMs;
	if (!isLimit && request.tif == TimeInForce::FOK) { return nullptr; }
	TimeInForce tif = isLimit ? request.tif : TimeInForce::IOC;
	double expiresAtMs = 0.0;
	switch (tif) {
	case TimeInForce::DAY:
		expiresAtMs = MarketCalendar::dayOrderExpiryMs(nowMs, request.sessions);
		break;
	case TimeInForce::GTC:
		expiresAtMs = MarketCalendar::dayOrderExpiryMs(
			nowMs + MarketCalendar::minutesToMs(GTC_MAX_DAYS * MarketCalendar::TOTAL_MINUTES_PER_DAY), request.sessions);
		break;
	case TimeInForce::GTD:
		if (isLimit && !(request.expiresAtMs > nowMs)) { return nullptr; }
		expiresAtMs = isLimit ? request.expiresAtMs : 0.0;
		break;
	case TimeInForce::IOC:
		break;
	case TimeInForce::FOK:
		break;
	}

	std::vector<Holding> reserved;
	double feeReserve = 0.0;

	if (request.side == OrderAction::BID) {
		if (isLimit) {
			// A limit bid escrows its whole cost up front, at its own price. A cent of slack
			// because the agent sized against buying power before the cost was rounded to the
			// cent (and an endowment is not cent-rounded), so rounding can carry a floor-sized
			// order up to half a cent past it. That must never get a valid order refused.
			double escrow = roundTo(request.price * request.volume);
			// With fees on, the bid also escrows its worst-case fees, rounded up to the cent, so
			// a cash account can never be driven below zero by what a fill costs
			if (Account::feesEnabled(*agent)) {
				double worst = Account::feeSchedule(*agent).worstCaseBuyFees(request.volume, request.price);
				feeReserve = std::ceil(worst / CASH_PRECISION - 1e-9) * CASH_PRECISION;
			}
			if (escrow + feeReserve > Account::buyingPower(*agent) + CASH_PRECISION) { return nullptr; }
			agent->updateCash(-escrow);
			if (feeReserve > 0.0) { agent->updateCash(-feeReserve); }
		}
		// A market bid escrows nothing and pays leg by leg, bounded by buying power in the
		// matching engine. See MatchingEngine::sweep for why that asymmetry is deliberate.
	}
	else {
		// An ask reserves the shares it is selling, so they cannot be sold twice
		if (request.volume > agent->getTotalHoldings()) { return nullptr; }
		reserved = agent->removeHoldings(int(request.volume));
	}

	std::shared_ptr<Order> order = std::make_shared<Order>(
		this->OB.makeId(ID_TYPE::ORDER),
		agent->id,
		isLimit ? request.price : -1.0,
		request.volume,
		nowMs,
		request.side,
		request.type,
		reserved,
		expiresAtMs,
		tif,
		request.sessions
	);
	order->feeReserve = feeReserve;

	// Fill or kill decides before anything trades, so a kill leaves the book untouched
	if (tif == TimeInForce::FOK && this->ME.fillableVolume(order) < order->volume) {
		this->killUnplaced(order, agent);
		return order;
	}

	this->ME.match(order);
	return order;
}

void Broker::killUnplaced(const std::shared_ptr<Order>& order, const std::shared_ptr<Agent>& agent) {
	order->status = OrderStatus::CANCELED;
	if (order->side == OrderAction::BID) {
		if (order->type == OrderType::LIMIT && order->volume > 0) { agent->updateCash(order->price * order->volume); }
		Account::releaseFeeReserve(*agent, *order);
	}
	else {
		for (Holding h : order->getReturnableShares()) { agent->upsertHolding(h); }
	}
}
void Broker::cancel(const std::shared_ptr<Order>& order, const std::shared_ptr<Agent>& agent) {
	if (order == nullptr || agent == nullptr) { return; }
	++this->stats.cancels;
	this->stats.restedMsSum += this->OB.clock->simTimeMs - order->timestamp;
	++this->stats.restedSamples;
	this->OB.cancelOrder(order, agent);
}

std::shared_ptr<Order> Broker::replace(const std::shared_ptr<Order>& order, const std::shared_ptr<Agent>& agent,
	double newPrice, unsigned int newVolume) {

	if (order == nullptr || agent == nullptr || order->agentId != agent->id) { return nullptr; }
	if (order->status != OrderStatus::OPEN || order->type != OrderType::LIMIT) { return nullptr; }
	if (newVolume == 0 || !(newPrice > 0.0)) { return nullptr; }

	const bool isBid = (order->side == OrderAction::BID);
	const auto& resting = isBid ? agent->activeBids : agent->activeAsks;
	if (resting.find(order->id) == resting.end()) { return nullptr; }

	const unsigned int oldVolume = order->volume;
	const double oldPrice = order->price;
	const bool priceChanged = (newPrice != oldPrice);
	if (!priceChanged && newVolume == oldVolume) { return order; }

	// ---- Can the account carry the new terms? Checked before anything moves. ----
	const double oldEscrow = oldPrice * oldVolume;   // what cancelOrder would refund
	const double newEscrow = roundTo(newPrice * newVolume);
	double newReserve = 0.0;
	if (isBid && Account::feesEnabled(*agent)) {
		double worst = Account::feeSchedule(*agent).worstCaseBuyFees(newVolume, newPrice);
		newReserve = std::ceil(worst / CASH_PRECISION - 1e-9) * CASH_PRECISION;
	}
	if (isBid) {
		if (newEscrow + newReserve > Account::buyingPower(*agent) + oldEscrow + order->feeReserve + CASH_PRECISION) {
			++this->stats.replacesRefused;
			return nullptr;
		}
	}
	else if (newVolume > oldVolume && (newVolume - oldVolume) > agent->getTotalHoldings()) {
		++this->stats.replacesRefused;
		return nullptr;
	}

	++this->stats.replaces;
	this->stats.restedMsSum += this->OB.clock->simTimeMs - order->timestamp;
	++this->stats.restedSamples;

	// ---- Size decrease at the same price: keeps its place in the queue ----
	// Volume is not part of the comparator key, so nothing needs re-sorting. Only a
	// DECREASE: a same-price increase is new size joining the queue, and goes to the back.
	if (!priceChanged && newVolume < oldVolume) {
		if (isBid) { agent->updateCash(oldPrice * (oldVolume - newVolume)); }
		else { for (const Holding& h : order->trimReserved(newVolume)) { agent->upsertHolding(h); } }
		order->volume = newVolume;
		++this->stats.replacesInPlace;
		return order;
	}

	// ---- Price change or size increase: loses priority ----
	// Out of the queue while its key is still the one it was sorted by
	this->OB.removeFromQueue(order);

	if (isBid) {
		agent->updateCash(oldEscrow);
		agent->updateCash(-newEscrow);
		if (order->feeReserve > 0.0 || newReserve > 0.0) {
			Account::releaseFeeReserve(*agent, *order);
			if (newReserve > 0.0) { agent->updateCash(-newReserve); }
			order->feeReserve = newReserve;
		}
	}
	else if (newVolume < oldVolume) {
		for (const Holding& h : order->trimReserved(newVolume)) { agent->upsertHolding(h); }
	}
	else {
		std::vector<Holding> lots = order->getReturnableShares();
		if (newVolume > oldVolume) {
			std::vector<Holding> added = agent->removeHoldings(int(newVolume - oldVolume));
			lots.insert(lots.end(), added.begin(), added.end());
		}
		order->reservedShares = lots;
	}

	order->price = newPrice;
	order->volume = newVolume;
	order->timestamp = this->OB.clock->simTimeMs;

	// Re-enters as any new order would: trades if it has become marketable, rests the rest.
	// It is still in the agent's active map, so if nothing of it rests it must leave there.
	this->ME.match(order);
	if (order->status != OrderStatus::OPEN) { agent->removeActiveOrder(order); }
	return order;
}

// ---- Margin and the trigger pump (OrderModelPlan Step 1.2) ----

void Broker::reset() {
	this->stats = BrokerStats();
	this->triggers.clear();
}

bool Broker::refreshMargin(const std::shared_ptr<Agent>& agent) {
	if (agent == nullptr) { return false; }
	double trigger = Account::liquidationPrice(*agent);
	if (trigger > 0.0) { this->triggers.setMargin(agent->id, TriggerBook::Side::SELL, trigger); }
	else { this->triggers.clearMargin(agent->id); }
	return Account::inMaintenanceViolation(*agent, this->OB.currentPrice)
		|| (Account::equity(*agent, this->OB.currentPrice) < 0.0 && agent->getTotalHoldings() == 0
			&& agent->activeBids.empty() && agent->activeAsks.empty());
}

void Broker::processTriggers() {
	if (this->OB.matchDepth > 0) { ++this->stats.reentrancyBlocked; return; }

	// Nothing is being watched and nothing has changed: the common case with margin off,
	// which must cost no more than this check
	if (this->OB.dirtyAccounts.empty() && this->triggers.empty()) {
		this->OB.printRangeValid = false;
		return;
	}

	int rounds = 0;
	for (; rounds < MAX_TRIGGER_ROUNDS; ++rounds) {
		std::vector<std::shared_ptr<Agent>> toLiquidate;
		std::unordered_set<std::string> seen;

		// 1. Accounts that changed: move their trigger, and catch any already in violation
		std::vector<std::string> dirty;
		dirty.swap(this->OB.dirtyAccounts);
		for (const std::string& id : dirty) {
			if (!seen.insert(id).second) { continue; }
			std::shared_ptr<Agent> agent = this->OB.getAgent(id);
			if (this->refreshMargin(agent)) { toLiquidate.push_back(agent); }
		}

		// 2. Whatever the prints since the last look crossed, in firing order
		if (this->OB.printRangeValid) {
			double low = this->OB.printLow, high = this->OB.printHigh;
			this->OB.printRangeValid = false;
			for (const TriggerEntry& entry : this->triggers.collect(low, high)) {
				if (entry.kind != TriggerKind::MARGIN) { continue; }
				if (!seen.insert(entry.agentId).second) { continue; }
				std::shared_ptr<Agent> agent = this->OB.getAgent(entry.agentId);
				if (agent != nullptr) { toLiquidate.push_back(agent); }
			}
		}

		if (toLiquidate.empty()) { break; }

		// 3. Meet the calls. Their prints and fills feed the next round.
		for (const std::shared_ptr<Agent>& agent : toLiquidate) { this->liquidate(agent); }
		++this->stats.triggerRounds;
	}

	if (rounds >= MAX_TRIGGER_ROUNDS) { ++this->stats.roundCapHits; }
	if (rounds > this->stats.maxRoundsInOnePump) { this->stats.maxRoundsInOnePump = rounds; }
}

void Broker::liquidate(const std::shared_ptr<Agent>& agent) {
	if (agent == nullptr) { return; }
	++this->stats.marginCalls;

	// Everything the account has working is cancelled first: the escrow comes back, nothing
	// can trade against the liquidation itself, and none of it changes the account's equity
	std::vector<std::shared_ptr<Order>> working;
	for (const auto& kv : agent->activeBids) { working.push_back(kv.second); }
	for (const auto& kv : agent->activeAsks) { working.push_back(kv.second); }
	for (const std::shared_ptr<Order>& order : working) { this->OB.cancelOrder(order, agent); }

	double price = this->OB.currentPrice;
	unsigned int shares = Account::liquidationShares(*agent, price);
	if (shares == 0) { this->writeOffIfInsolvent(agent); return; }

	// A market order in the regular session. Outside it, where market orders are not
	// accepted, an IOC limit through the best bid by up to LIQUIDATION_OUTSIDE_SLIP.
	OrderRequest request{ OrderAction::ASK, OrderType::MARKET };
	request.volume = shares;
	request.tif = TimeInForce::IOC;
	request.origin = OrderOrigin::LIQUIDATION;
	if (this->OB.session != Session::REGULAR) {
		std::vector<std::shared_ptr<Order>> bids = this->OB.peekBestN(OrderAction::BID, 1);
		if (bids.empty() || bids[0] == nullptr) { return; }   // nobody to sell to; the next look retries
		request.type = OrderType::LIMIT;
		double floor = bids[0]->price * (1.0 - LIQUIDATION_OUTSIDE_SLIP);
		double precision = (floor < 1.00) ? 0.0001 : 0.01;
		request.price = std::max(roundTo(floor, precision), precision);
	}

	++this->stats.liquidationOrders;
	this->submit(request, agent);
	this->writeOffIfInsolvent(agent);
}

void Broker::writeOffIfInsolvent(const std::shared_ptr<Agent>& agent) {
	if (agent == nullptr) { return; }
	if (agent->getTotalHoldings() > 0 || !agent->activeBids.empty() || !agent->activeAsks.empty()) { return; }
	if (agent->cash >= 0.0) { return; }

	// Nothing left to sell and still owing: the broker eats it
	double loss = -agent->cash;
	this->OB.ledger.brokerLosses += loss;
	agent->updateCash(loss);
	++this->stats.writeOffs;
	if (!isLifecycleStatus(agent->status)) { agent->status = AgentStatus::BANKRUPT; }
}

void Broker::accrueMarginInterest() {
	if (!this->OB.features.margin.enabled) { return; }
	++this->stats.interestDays;
	for (const auto& kv : this->OB.agents) {
		const std::shared_ptr<Agent>& agent = kv.second;
		if (agent == nullptr) { continue; }
		double debit = Account::debitBalance(*agent);
		if (debit <= 0.0) { continue; }
		double interest = roundTo(debit * Account::feeSchedule(*agent).marginApr / 360.0);
		if (interest <= 0.0) { continue; }
		agent->updateCash(-interest);
		this->OB.ledger.marginInterest += interest;
		this->stats.interestCharged += interest;
		this->OB.dirtyAccounts.push_back(agent->id);
	}
}
