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

Broker::Broker(OrderBook& ob, MatchingEngine& me) : OB(ob), ME(me) {}

std::shared_ptr<Order> Broker::submit(const OrderRequest& request, const std::shared_ptr<Agent>& agent) {
	if (agent == nullptr || request.empty()) { return nullptr; }
	if (request.side != OrderAction::BID && request.side != OrderAction::ASK) { return nullptr; }

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

	if (request.side == OrderAction::BID) {
		if (isLimit) {
			// A limit bid escrows its whole cost up front, at its own price. A cent of slack
			// because the agent sized against buying power before the cost was rounded to the
			// cent (and an endowment is not cent-rounded), so rounding can carry a floor-sized
			// order up to half a cent past it. That must never get a valid order refused.
			double escrow = roundTo(request.price * request.volume);
			if (escrow > Account::buyingPower(*agent) + CASH_PRECISION) { return nullptr; }
			agent->updateCash(-escrow);
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
	}
	else {
		for (Holding h : order->getReturnableShares()) { agent->upsertHolding(h); }
	}
}
void Broker::cancel(const std::shared_ptr<Order>& order, const std::shared_ptr<Agent>& agent) {
	if (order == nullptr || agent == nullptr) { return; }
	this->OB.cancelOrder(order, agent);
}
