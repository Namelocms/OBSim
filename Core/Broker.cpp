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
