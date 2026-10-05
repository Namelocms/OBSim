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
#include <algorithm>
#include "include/Ledger.h"
#include "include/Luld.h"

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
	std::shared_ptr<Order> order = request.isStop() ? this->hold(request, agent) : this->place(request, agent);
	if (order == nullptr) { ++this->stats.refused; }
	else { ++this->stats.submitted; }
	return order;
}

std::shared_ptr<Order> Broker::placeTagged(const OrderRequest& request, const std::shared_ptr<Agent>& agent, const std::string& groupId) {
	return this->place(request, agent, groupId);
}

std::shared_ptr<Order> Broker::place(const OrderRequest& request, const std::shared_ptr<Agent>& agent, const std::string& groupId) {

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

	// On-open and on-close orders wait for their cross (OrderModelPlan Step 3.1). An on-open
	// order is taken any time before the open; an on-close one until the 15:50 cutoff.
	const bool auction = (request.tif == TimeInForce::OPG || request.tif == TimeInForce::CLS);
	if (auction) {
		if (!this->OB.features.auctions.enabled) { return nullptr; }
		if (request.tif == TimeInForce::OPG && this->OB.session == Session::REGULAR) { return nullptr; }
		if (request.tif == TimeInForce::CLS) {
			Session s = this->OB.session;
			if (s != Session::PREMARKET && s != Session::REGULAR) { return nullptr; }
			double close = MarketCalendar::sessionOpenMs(Session::REGULAR, MarketCalendar::dayIndex(nowMs))
				+ MarketCalendar::sessionLengthMs(Session::REGULAR);
			if (nowMs >= close - MarketCalendar::minutesToMs(CLOSE_ORDER_CUTOFF_MINUTES)) { return nullptr; }
		}
	}
	// A market buy waiting for a cross is escrowed at its collar, which is also the most it pays
	const double collarPrice = roundTo(this->OB.currentPrice * (1.0 + AUCTION_MARKET_COLLAR),
		(this->OB.currentPrice < 1.0) ? 0.0001 : 0.01);
	const bool escrowed = isLimit || (auction && request.side == OrderAction::BID);
	const double escrowPrice = isLimit ? request.price : collarPrice;

	TimeInForce tif = (isLimit || auction) ? request.tif : TimeInForce::IOC;
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
	case TimeInForce::OPG:
	case TimeInForce::CLS:
		break;   // cancelled by the cross if it does not fill there
	}

	std::vector<Holding> reserved;
	double feeReserve = 0.0;

	// A bid for no more than the account's uncovered short is a buy to cover. It reduces risk,
	// so a broker accepts it whatever the buying power -- in a margin account any shortfall
	// is simply a debit -- which is also what lets a margin call on a short be met at all.
	const bool covering = (request.side == OrderAction::BID) && agent->shortShares > 0
		&& request.volume <= Account::coverableShares(*agent);

	if (request.side == OrderAction::BID) {
		if (escrowed) {
			// A limit bid escrows its whole cost up front, at its own price. A cent of slack
			// because the agent sized against buying power before the cost was rounded to the
			// cent (and an endowment is not cent-rounded), so rounding can carry a floor-sized
			// order up to half a cent past it. That must never get a valid order refused.
			double escrow = roundTo(escrowPrice * request.volume);
			// With fees on, the bid also escrows its worst-case fees, rounded up to the cent, so
			// a cash account can never be driven below zero by what a fill costs
			if (Account::feesEnabled(*agent)) {
				double worst = Account::feeSchedule(*agent).worstCaseBuyFees(request.volume, escrowPrice);
				feeReserve = std::ceil(worst / CASH_PRECISION - 1e-9) * CASH_PRECISION;
			}
			if (!covering && escrow + feeReserve > Account::buyingPower(*agent) + CASH_PRECISION) { return nullptr; }
			agent->updateCash(-escrow);
			if (feeReserve > 0.0) { agent->updateCash(-feeReserve); }
		}
		// A market bid escrows nothing and pays leg by leg, bounded by buying power in the
		// matching engine. See MatchingEngine::sweep for why that asymmetry is deliberate.
	}
	else if (request.mark == SaleMark::LONG) {
		// An ask reserves the shares it is selling, so they cannot be sold twice
		if (request.volume > agent->getTotalHoldings()) { return nullptr; }
		reserved = agent->removeHoldings(int(request.volume));
	}
	else {
		// A short sale. Never both: an account holding shares sells them first. It needs margin
		// for the new short, and either a locate or a market maker's exemption.
		if (!Account::shortingEnabled(*agent) || agent->getTotalHoldings() > 0) { return nullptr; }
		if (request.volume > Account::shortCapacity(*agent)) { return nullptr; }
		if (request.mark == SaleMark::SHORT_EXEMPT) {
			if (!Account::isExemptMarketMaker(*agent)) { return nullptr; }
		}
		else if (!this->OB.lending.locate(request.volume)) { return nullptr; }
	}

	std::shared_ptr<Order> order = std::make_shared<Order>(
		this->OB.makeId(ID_TYPE::ORDER),
		agent->id,
		isLimit ? request.price : ((auction && request.side == OrderAction::BID) ? collarPrice : -1.0),
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
	order->mark = (request.side == OrderAction::ASK) ? request.mark : SaleMark::LONG;
	order->groupId = groupId;

	// Waits for its cross instead of matching now
	if (auction) {
		this->OB.queueForAuction(order);
		agent->upsertActiveOrder(order);
		return order;
	}

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
		if (order->mark == SaleMark::SHORT) { this->OB.lending.releaseLocate(order->volume); }
	}
}
void Broker::cancel(const std::shared_ptr<Order>& order, const std::shared_ptr<Agent>& agent) {
	if (order == nullptr || agent == nullptr) { return; }
	if (order->held) { this->cancelHeld(order, agent); return; }
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
	else if (order->isShortSale()) {
		if (newVolume > oldVolume) {
			unsigned int more = newVolume - oldVolume;
			bool carried = more <= Account::shortCapacity(*agent)
				&& (order->mark == SaleMark::SHORT_EXEMPT || this->OB.lending.locate(more));
			if (!carried) { ++this->stats.replacesRefused; return nullptr; }
		}
		else if (order->mark == SaleMark::SHORT) { this->OB.lending.releaseLocate(oldVolume - newVolume); }
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
		else if (!order->isShortSale()) { for (const Holding& h : order->trimReserved(newVolume)) { agent->upsertHolding(h); } }
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
	else if (order->isShortSale()) {
		// No shares behind a short sale; its locate was adjusted above
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
	this->buyIns.clear();
	this->stopTriggers.clear();
	this->held.clear();
	this->trailingSells.clear();
	this->trailingBuys.clear();
	this->groups.clear();
}

// ---- Stops (OrderModelPlan Step 2.1) ----

bool Broker::stopsActive() const {
	return this->OB.features.stops.extendedHours || this->OB.session == Session::REGULAR;
}

double Broker::trailingTrigger(const Order& order, double extreme) {
	bool sell = (order.side == OrderAction::ASK);
	double trigger = (order.trailAmount > 0.0)
		? (sell ? extreme - order.trailAmount : extreme + order.trailAmount)
		: (sell ? extreme * (1.0 - order.trailPercent) : extreme * (1.0 + order.trailPercent));
	double precision = (trigger < 1.00) ? 0.0001 : 0.01;
	return roundTo(trigger, precision);
}

std::shared_ptr<Order> Broker::hold(const OrderRequest& request, const std::shared_ptr<Agent>& agent, bool reserveShares) {
	if (request.side != OrderAction::BID && request.side != OrderAction::ASK) { return nullptr; }
	const bool sell = (request.side == OrderAction::ASK);
	const bool trailing = request.trailAmount > 0.0 || request.trailPercent > 0.0;
	const double now = this->OB.clock->simTimeMs;
	const double last = this->OB.currentPrice;

	// A stop-limit needs its limit, and a stop lives until its time in force says otherwise:
	// IOC and FOK mean nothing for an order that waits by design
	if (request.type == OrderType::LIMIT && !(request.price > 0.0)) { return nullptr; }
	if (request.tif == TimeInForce::IOC || request.tif == TimeInForce::FOK) { return nullptr; }
	if ((request.sessions & SESSIONS_ALL) == 0) { return nullptr; }
	if (trailing && request.trailPercent >= 1.0) { return nullptr; }

	// A protective stop: sells come out of a long position, which they reserve, and must sit
	// below the market; buys sit above it. A stop already through the market would simply be
	// a market order, so it is refused rather than fired at once.
	double trigger = request.stopPrice;
	if (trailing) {
		Order probe("", agent->id, 0.0, 1, now, request.side, request.type);
		probe.trailAmount = request.trailAmount;
		probe.trailPercent = request.trailPercent;
		trigger = trailingTrigger(probe, last);
	}
	if (!(trigger > 0.0)) { return nullptr; }
	if (sell ? !(trigger < last) : !(trigger > last)) { return nullptr; }

	std::vector<Holding> reserved;
	if (sell) {
		if (request.mark != SaleMark::LONG) { return nullptr; }
		if (request.volume > agent->getTotalHoldings()) { return nullptr; }
		if (reserveShares) { reserved = agent->removeHoldings(int(request.volume)); }
	}

	double expiresAtMs = 0.0;
	switch (request.tif) {
	case TimeInForce::DAY: expiresAtMs = MarketCalendar::dayOrderExpiryMs(now, request.sessions); break;
	case TimeInForce::GTC:
		expiresAtMs = MarketCalendar::dayOrderExpiryMs(
			now + MarketCalendar::minutesToMs(GTC_MAX_DAYS * MarketCalendar::TOTAL_MINUTES_PER_DAY), request.sessions);
		break;
	default:
		if (!(request.expiresAtMs > now)) {
			for (Holding h : reserved) { agent->upsertHolding(h); }
			return nullptr;
		}
		expiresAtMs = request.expiresAtMs;
		break;
	}

	std::shared_ptr<Order> order = std::make_shared<Order>(
		this->OB.makeId(ID_TYPE::ORDER), agent->id,
		(request.type == OrderType::LIMIT) ? request.price : -1.0,
		request.volume, now, request.side, request.type, reserved, expiresAtMs, request.tif, request.sessions);
	order->held = true;
	order->stopPrice = trigger;
	order->trailAmount = request.trailAmount;
	order->trailPercent = request.trailPercent;
	order->trailExtreme = last;

	agent->heldOrders[order->id] = order;
	this->held[order->id] = order;
	this->stopTriggers.setOrder(order->id, agent->id, sell ? TriggerBook::Side::SELL : TriggerBook::Side::BUY, trigger);
	if (trailing) {
		if (sell) { this->trailingSells.emplace(last, order->id); }
		else { this->trailingBuys.emplace(last, order->id); }
	}
	++this->stats.stopsPlaced;
	return order;
}

static void eraseTrailing(std::multimap<double, std::string>& sells,
	std::multimap<double, std::string, std::greater<double>>& buys, const Order& order) {
	if (!order.isTrailing()) { return; }
	if (order.side == OrderAction::ASK) {
		auto range = sells.equal_range(order.trailExtreme);
		for (auto it = range.first; it != range.second; ++it) { if (it->second == order.id) { sells.erase(it); return; } }
	}
	else {
		auto range = buys.equal_range(order.trailExtreme);
		for (auto it = range.first; it != range.second; ++it) { if (it->second == order.id) { buys.erase(it); return; } }
	}
}

void Broker::cancelHeld(const std::shared_ptr<Order>& order, const std::shared_ptr<Agent>& agent) {
	if (order == nullptr || !order->held) { return; }
	if (!order->groupId.empty()) {
		this->OB.groupEvents.push_back({ GroupEvent::Type::CANCEL, order->groupId, order->id, order->volume });
	}
	this->stopTriggers.clearOrder(order->id);
	eraseTrailing(this->trailingSells, this->trailingBuys, *order);
	this->held.erase(order->id);
	order->held = false;
	order->status = OrderStatus::CANCELED;
	if (agent != nullptr) {
		for (Holding h : order->getReturnableShares()) { agent->upsertHolding(h); }
		agent->heldOrders.erase(order->id);
	}
	++this->stats.stopsCancelled;
}

void Broker::trailTo(double price) {
	// Sells trail the high: every one whose high is below this print has a new high
	while (!this->trailingSells.empty() && this->trailingSells.begin()->first < price) {
		std::string id = this->trailingSells.begin()->second;
		this->trailingSells.erase(this->trailingSells.begin());
		auto found = this->held.find(id);
		if (found == this->held.end()) { continue; }
		Order& order = *found->second;
		order.trailExtreme = price;
		order.stopPrice = trailingTrigger(order, price);
		this->stopTriggers.setOrder(order.id, order.agentId, TriggerBook::Side::SELL, order.stopPrice);
		this->trailingSells.emplace(price, id);
		++this->stats.trailMoves;
	}
	// Buys trail the low
	while (!this->trailingBuys.empty() && this->trailingBuys.begin()->first > price) {
		std::string id = this->trailingBuys.begin()->second;
		this->trailingBuys.erase(this->trailingBuys.begin());
		auto found = this->held.find(id);
		if (found == this->held.end()) { continue; }
		Order& order = *found->second;
		order.trailExtreme = price;
		order.stopPrice = trailingTrigger(order, price);
		this->stopTriggers.setOrder(order.id, order.agentId, TriggerBook::Side::BUY, order.stopPrice);
		this->trailingBuys.emplace(price, id);
		++this->stats.trailMoves;
	}
}

void Broker::release(const std::shared_ptr<Order>& order) {
	if (order == nullptr || !order->held) { return; }
	if (!order->groupId.empty()) {
		auto group = this->groups.find(order->groupId);
		if (group != this->groups.end() && group->second->heldLegId == order->id) {
			this->takeOverFromBookLeg(*group->second, order);
		}
	}
	std::shared_ptr<Agent> agent = this->OB.getAgent(order->agentId);
	eraseTrailing(this->trailingSells, this->trailingBuys, *order);
	this->held.erase(order->id);
	order->held = false;
	if (agent == nullptr) { order->status = OrderStatus::CANCELED; return; }
	agent->heldOrders.erase(order->id);
	++this->stats.stopsTriggered;

	// It enters the book now, as a new order would, behind everything already there
	order->timestamp = this->OB.clock->simTimeMs;

	// A sell stop's shares were reserved when it was placed. A buy stop reserved nothing, as
	// at a real broker, so it is paid for now -- unless it covers a short, which a broker
	// accepts whatever the buying power.
	if (order->side == OrderAction::BID) {
		const bool covering = agent->shortShares > 0 && order->volume <= Account::coverableShares(*agent);
		if (order->type == OrderType::LIMIT) {
			double escrow = roundTo(order->price * order->volume);
			double reserve = 0.0;
			if (Account::feesEnabled(*agent)) {
				double worst = Account::feeSchedule(*agent).worstCaseBuyFees(order->volume, order->price);
				reserve = std::ceil(worst / CASH_PRECISION - 1e-9) * CASH_PRECISION;
			}
			if (!covering && escrow + reserve > Account::buyingPower(*agent) + CASH_PRECISION) {
				order->status = OrderStatus::CANCELED;
				++this->stats.stopsRefusedAtTrigger;
				return;
			}
			agent->updateCash(-escrow);
			if (reserve > 0.0) { agent->updateCash(-reserve); }
			order->feeReserve = reserve;
		}
		else if (!covering && Account::affordableVolume(*agent, this->OB.currentPrice) < 1) {
			order->status = OrderStatus::CANCELED;
			++this->stats.stopsRefusedAtTrigger;
			return;
		}
	}

	this->ME.match(order);
}

unsigned int Broker::expireHeld(double nowMs) {
	std::vector<std::shared_ptr<Order>> due;
	for (const auto& kv : this->held) {
		if (kv.second->expiresAtMs > 0.0 && kv.second->expiresAtMs <= nowMs) { due.push_back(kv.second); }
	}
	std::sort(due.begin(), due.end(), [](const auto& a, const auto& b) { return a->id < b->id; });
	for (const std::shared_ptr<Order>& order : due) {
		this->cancelHeld(order, this->OB.getAgent(order->agentId));
		--this->stats.stopsCancelled;
		++this->stats.stopsExpired;
	}
	return (unsigned int)due.size();
}

void Broker::buyBack(const std::shared_ptr<Agent>& agent, unsigned int shares, OrderOrigin origin) {
	// Whatever it has bid already is cancelled, so the new order is the whole cover and the
	// covering check sees all of the short as uncovered
	std::vector<std::shared_ptr<Order>> bids;
	for (const auto& kv : agent->activeBids) { bids.push_back(kv.second); }
	for (const std::shared_ptr<Order>& order : bids) { this->OB.cancelOrder(order, agent); }

	shares = std::min(shares, agent->shortShares);
	if (shares == 0) { return; }

	// A market order in the regular session; outside it an IOC limit up to
	// LIQUIDATION_OUTSIDE_SLIP through the best offer
	OrderRequest request{ OrderAction::BID, OrderType::MARKET };
	request.volume = shares;
	request.tif = TimeInForce::IOC;
	request.origin = origin;
	if (this->OB.session != Session::REGULAR) {
		std::vector<std::shared_ptr<Order>> asks = this->OB.peekBestN(OrderAction::ASK, 1);
		if (asks.empty() || asks[0] == nullptr) { return; }
		request.type = OrderType::LIMIT;
		double cap = asks[0]->price * (1.0 + LIQUIDATION_OUTSIDE_SLIP);
		request.price = roundTo(cap, (cap < 1.00) ? 0.0001 : 0.01);
	}
	this->submit(request, agent);
}

void Broker::buyIn(const std::shared_ptr<Agent>& agent, unsigned int shares) {
	++this->stats.buyInOrders;
	this->buyBack(agent, shares, OrderOrigin::LIQUIDATION);
}

void Broker::recallLoans() {
	if (!this->OB.features.shorting.enabled) { return; }
	for (const auto& recall : this->OB.lending.recallsNeeded()) {
		std::shared_ptr<Agent> agent = this->OB.getAgent(recall.first);
		if (agent == nullptr) { continue; }
		// The shares go back to the lender who sold them; the borrower still owes them, so the
		// loan becomes a fail it has to buy back in now
		this->OB.lending.returnLoan(*agent, recall.second);
		agent->buyInDue = std::min<unsigned int>(agent->shortShares, agent->buyInDue + recall.second);
		this->buyIns.insert(agent->id);
		this->stats.recalledShares += recall.second;
	}
}

void Broker::borrowForFails() {
	if (!this->OB.features.shorting.enabled) { return; }
	std::vector<std::shared_ptr<Agent>> failing;
	for (const auto& kv : this->OB.agents) {
		if (kv.second != nullptr && kv.second->shortShares > kv.second->borrowedShares) { failing.push_back(kv.second); }
	}
	std::sort(failing.begin(), failing.end(), [](const auto& a, const auto& b) { return a->id < b->id; });
	for (const std::shared_ptr<Agent>& agent : failing) {
		unsigned int fails = agent->shortShares - agent->borrowedShares;
		this->stats.failsBorrowed += this->OB.lending.borrowUpTo(*agent, fails, this->OB.clock->simTimeMs);
	}
}

void Broker::closeOutFails() {
	if (!this->OB.features.shorting.enabled) { return; }
	for (const auto& kv : this->OB.agents) {
		const std::shared_ptr<Agent>& agent = kv.second;
		if (agent == nullptr || agent->shortShares <= agent->borrowedShares) { continue; }
		unsigned int fails = agent->shortShares - agent->borrowedShares;
		agent->buyInDue = std::max(agent->buyInDue, fails);
		this->buyIns.insert(agent->id);
		this->stats.failsClosedOut += fails;
	}
}

bool Broker::refreshMargin(const std::shared_ptr<Agent>& agent) {
	if (agent == nullptr) { return false; }
	if (agent->shortShares > 0) {
		// A short is called as the price RISES through its trigger
		double trigger = Account::shortLiquidationPrice(*agent);
		if (trigger > 0.0) { this->triggers.setMargin(agent->id, TriggerBook::Side::BUY, trigger); }
		else { this->triggers.clearMargin(agent->id); }
	}
	else {
		double trigger = Account::liquidationPrice(*agent);
		if (trigger > 0.0) { this->triggers.setMargin(agent->id, TriggerBook::Side::SELL, trigger); }
		else { this->triggers.clearMargin(agent->id); }
	}
	if (agent->buyInDue > 0) { this->buyIns.insert(agent->id); }
	return Account::inMaintenanceViolation(*agent, this->OB.currentPrice)
		|| (Account::equity(*agent, this->OB.currentPrice) < 0.0 && agent->getTotalHoldings() == 0
			&& agent->shortShares == 0 && agent->activeBids.empty() && agent->activeAsks.empty());
}

void Broker::processTriggers() {
	if (this->OB.matchDepth > 0) { ++this->stats.reentrancyBlocked; return; }
	// During a LULD trading pause nothing trades, so nothing is acted on: stops and margin
	// calls wait for the reopening cross's print
	if (this->OB.luld.paused) { return; }
	this->runPump();
	// After the pump has settled, see whether the book is sitting at a band
	if (this->OB.luld.active && Luld::checkLimitState(this->OB, this->OB.clock->simTimeMs)) {
		++this->stats.tradingPauses;
	}
}

void Broker::runPump() {

	// Nothing is being watched and nothing has changed: the common case with every mechanism
	// off, which must cost no more than this check
	if (this->OB.dirtyAccounts.empty() && this->triggers.empty() && this->buyIns.empty()
		&& this->stopTriggers.empty() && this->OB.groupEvents.empty()) {
		this->OB.pendingPrints.clear();
		return;
	}

	int rounds = 0;
	std::unordered_set<std::string> attemptedBuyIn;   // one attempt per account per pump
	for (; rounds < MAX_TRIGGER_ROUNDS; ++rounds) {
		std::vector<std::shared_ptr<Agent>> toLiquidate;
		std::vector<std::shared_ptr<Order>> toRelease;
		std::unordered_set<std::string> seen;

		// 0. OCO and bracket members that filled or were cancelled since the last look
		bool groupsMoved = !this->OB.groupEvents.empty();
		this->processGroupEvents();

		// 1. Accounts that changed: move their trigger, and catch any already in violation
		std::vector<std::string> dirty;
		dirty.swap(this->OB.dirtyAccounts);
		for (const std::string& id : dirty) {
			if (!seen.insert(id).second) { continue; }
			std::shared_ptr<Agent> agent = this->OB.getAgent(id);
			if (this->refreshMargin(agent)) { toLiquidate.push_back(agent); }
		}

		// 2. The prints since the last look, one at a time in the order they printed: each moves
		//    the trailing stops it sets a new extreme for, then fires whatever it reached
		std::vector<double> prints;
		prints.swap(this->OB.pendingPrints);
		const bool stopsLive = this->stopsActive();
		for (double p : prints) {
			if (stopsLive) {
				this->trailTo(p);
				for (const TriggerEntry& entry : this->stopTriggers.collect(p, p)) {
					auto found = this->held.find(entry.orderId);
					if (found != this->held.end()) { toRelease.push_back(found->second); }
				}
			}
			for (const TriggerEntry& entry : this->triggers.collect(p, p)) {
				if (entry.kind != TriggerKind::MARGIN) { continue; }
				if (!seen.insert(entry.agentId).second) { continue; }
				std::shared_ptr<Agent> agent = this->OB.getAgent(entry.agentId);
				if (agent != nullptr) { toLiquidate.push_back(agent); }
			}
		}

		// 3. Loans the lenders have sold out from under: recall them, newest first, and the
		//    borrowers must buy back in (OrderModelPlan Step 1.3)
		this->recallLoans();

		// 4. Buy-ins not yet attempted in this pump
		std::vector<std::shared_ptr<Agent>> toBuyIn;
		for (const std::string& id : this->buyIns) {
			if (seen.count(id) || attemptedBuyIn.count(id)) { continue; }
			std::shared_ptr<Agent> agent = this->OB.getAgent(id);
			if (agent != nullptr && agent->buyInDue > 0) { toBuyIn.push_back(agent); }
		}
		std::sort(toBuyIn.begin(), toBuyIn.end(), [](const auto& a, const auto& b) { return a->id < b->id; });

		if (toLiquidate.empty() && toBuyIn.empty() && toRelease.empty()) {
			// Group work can itself trade (a bracket's children going in) and so print; go round
			// once more if it did, otherwise this pump is done
			if (groupsMoved && (!this->OB.pendingPrints.empty() || !this->OB.groupEvents.empty())) { continue; }
			break;
		}

		// 5. Act. Margin calls first -- the broker protecting its loans -- then the stops in the
		//    order their prints reached them, then buy-ins. Every print and fill feeds the
		//    next round, which is how a cascade happens.
		for (const std::shared_ptr<Agent>& agent : toLiquidate) { this->liquidate(agent); }
		for (const std::shared_ptr<Order>& order : toRelease) { this->release(order); }
		for (const std::shared_ptr<Agent>& agent : toBuyIn) {
			attemptedBuyIn.insert(agent->id);
			this->buyIn(agent, agent->buyInDue);
		}
		++this->stats.triggerRounds;
	}

	// Accounts whose buy-in is done leave the set
	for (auto it = this->buyIns.begin(); it != this->buyIns.end();) {
		std::shared_ptr<Agent> agent = this->OB.getAgent(*it);
		if (agent == nullptr || agent->buyInDue == 0) { it = this->buyIns.erase(it); } else { ++it; }
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
	// A held stop's reserved shares are needed for the liquidation itself
	std::vector<std::shared_ptr<Order>> stops;
	for (const auto& kv : agent->heldOrders) { stops.push_back(kv.second); }
	for (const std::shared_ptr<Order>& order : stops) { this->cancelHeld(order, agent); }

	double price = this->OB.currentPrice;

	// A short is met by buying back enough to restore its opening requirement
	if (agent->shortShares > 0) {
		unsigned int cover = Account::shortCoverShares(*agent, price);
		if (cover > 0) {
			++this->stats.liquidationOrders;
			this->buyBack(agent, cover, OrderOrigin::LIQUIDATION);
		}
		this->writeOffIfInsolvent(agent);
		return;
	}

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
	if (agent->getTotalHoldings() > 0 || agent->shortShares > 0 || !agent->activeBids.empty() || !agent->activeAsks.empty()
		|| !agent->heldOrders.empty()) { return; }
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

// ---- Lending (OrderModelPlan Step 1.3) ----

void Broker::accrueBorrowFees() {
	if (!this->OB.features.shorting.enabled || !this->OB.features.margin.enabled) { return; }
	++this->stats.borrowFeeDays;

	// The once-a-day rebuild corrects contributions that drifted as the price moved retail
	// accounts across the margin line without them trading
	StockLoan& desk = this->OB.lending;
	desk.rebuildSupply(this->OB.agents);

	double price = this->OB.currentPrice;
	double rate = StockLoan::feeRate(desk.utilisation());

	// Charge borrowers, in id order so the run is reproducible whatever the map's hash order
	std::vector<std::shared_ptr<Agent>> borrowers, lenders;
	for (const auto& kv : this->OB.agents) {
		if (kv.second == nullptr) { continue; }
		if (kv.second->borrowedShares > 0) { borrowers.push_back(kv.second); }
		if (desk.institutionalContribution(kv.first) > 0.0) { lenders.push_back(kv.second); }
	}
	auto byId = [](const std::shared_ptr<Agent>& a, const std::shared_ptr<Agent>& b) { return a->id < b->id; };
	std::sort(borrowers.begin(), borrowers.end(), byId);
	std::sort(lenders.begin(), lenders.end(), byId);

	double charged = 0.0;
	for (const std::shared_ptr<Agent>& b : borrowers) {
		double fee = roundTo(double(b->borrowedShares) * price * rate / 360.0);
		if (fee <= 0.0) { continue; }
		b->updateCash(-fee);
		charged += fee;
		this->OB.dirtyAccounts.push_back(b->id);
	}
	if (charged <= 0.0) { return; }
	this->stats.borrowFeesCharged += charged;

	// Institutional lenders earn their share of the pool's fee, less the programme's cut.
	// Retail shares were rehypothecated: that part of the fee is the broker's.
	double supply = desk.supply();
	double instShare = (supply > 0.0) ? desk.institutionalSupply / supply : 0.0;
	double toLenders = charged * instShare * (1.0 - BORROW_PROGRAMME_SHARE);
	double paid = 0.0;
	if (desk.institutionalSupply > 0.0) {
		for (const std::shared_ptr<Agent>& l : lenders) {
			double cut = roundTo(toLenders * desk.institutionalContribution(l->id) / desk.institutionalSupply);
			if (cut <= 0.0) { continue; }
			l->updateCash(cut);
			paid += cut;
			this->OB.dirtyAccounts.push_back(l->id);
		}
	}
	this->stats.borrowFeesToLenders += paid;
	// Whatever was not paid out, rounding included, stayed with the house
	this->OB.ledger.borrowFees += charged - paid;
}

// ---- Contingent orders (OrderModelPlan Step 2.2) ----

std::shared_ptr<Order> Broker::holdShared(OrderRequest request, const std::shared_ptr<Agent>& agent, const std::string& groupId) {
	// Every check hold() makes, but a sell leg does not reserve: its sibling in the book holds
	// the shares, and two reservations of one position would sell it twice
	std::shared_ptr<Order> order = this->hold(request, agent, false);
	if (order != nullptr) {
		order->groupId = groupId;
		if (order->side == OrderAction::ASK) { order->reservedByGroup = true; }
	}
	return order;
}

std::shared_ptr<OrderGroup> Broker::submitOco(const OrderRequest& limitLeg, const OrderRequest& stopLeg,
	const std::shared_ptr<Agent>& agent) {
	if (agent == nullptr) { return nullptr; }
	if (limitLeg.isStop() || !stopLeg.isStop() || limitLeg.type != OrderType::LIMIT) { return nullptr; }
	if (limitLeg.side != stopLeg.side || limitLeg.volume != stopLeg.volume || limitLeg.volume == 0) { return nullptr; }

	auto group = std::make_shared<OrderGroup>();
	group->id = "G-" + this->OB.makeId(ID_TYPE::ORDER).substr(2);
	group->kind = OrderGroup::Kind::OCO;
	group->agentId = agent->id;

	// The stop goes first: if it is refused nothing has been placed. Then the limit leg, which
	// takes the real reservation.
	std::shared_ptr<Order> stop = this->holdShared(stopLeg, agent, group->id);
	if (stop == nullptr) { ++this->stats.refused; return nullptr; }
	std::shared_ptr<Order> book = this->place(limitLeg, agent);
	if (book == nullptr) {
		this->cancelHeld(stop, agent);
		this->OB.groupEvents.clear();
		++this->stats.refused;
		return nullptr;
	}
	book->groupId = group->id;
	group->bookLegId = book->id;
	group->heldLegId = stop->id;
	this->groups[group->id] = group;
	++this->stats.groupsCreated;
	this->stats.submitted += 2;

	// The limit leg may already have traded on arrival
	if (book->status != OrderStatus::OPEN) {
		unsigned int filled = book->entryVolume - book->volume;
		if (filled > 0) { this->OB.groupEvents.push_back({ GroupEvent::Type::FILL, group->id, book->id, filled }); }
	}
	return group;
}

std::shared_ptr<OrderGroup> Broker::submitBracket(const BracketRequest& request, const std::shared_ptr<Agent>& agent) {
	if (agent == nullptr || request.entry.isStop()) { return nullptr; }
	const bool longEntry = (request.entry.side == OrderAction::BID);
	double ref = (request.entry.type == OrderType::LIMIT) ? request.entry.price : this->OB.currentPrice;
	// A long bracket takes profit above and stops out below; a short one the other way round
	bool shaped = longEntry
		? (request.takeProfit > ref && request.stopLoss > 0.0 && request.stopLoss < ref)
		: (request.takeProfit > 0.0 && request.takeProfit < ref && request.stopLoss > ref);
	if (!shaped) { return nullptr; }

	auto group = std::make_shared<OrderGroup>();
	group->id = "G-" + this->OB.makeId(ID_TYPE::ORDER).substr(2);
	group->kind = OrderGroup::Kind::BRACKET;
	group->agentId = agent->id;
	group->childSide = longEntry ? OrderAction::ASK : OrderAction::BID;
	group->takeProfit = request.takeProfit;
	group->stopLoss = request.stopLoss;
	group->childTif = request.childTif;
	this->groups[group->id] = group;

	// Placed through place(), not submit(), so the group id is on it before it can trade
	std::shared_ptr<Order> entry = this->placeTagged(request.entry, agent, group->id);
	if (entry == nullptr) { this->groups.erase(group->id); ++this->stats.refused; return nullptr; }
	group->parentId = entry->id;
	++this->stats.groupsCreated;
	++this->stats.submitted;
	return group;
}

void Broker::processGroupEvents() {
	// Handling an event can raise more (a cancelled sibling records its own), so drain until dry
	for (int guard = 0; guard < 16 && !this->OB.groupEvents.empty(); ++guard) {
		std::vector<GroupEvent> events;
		events.swap(this->OB.groupEvents);
		for (const GroupEvent& event : events) {
			auto found = this->groups.find(event.groupId);
			if (found == this->groups.end()) { continue; }
			std::shared_ptr<OrderGroup> group = found->second;   // keep it alive through erase
			std::shared_ptr<Agent> agent = this->OB.getAgent(group->agentId);
			if (agent == nullptr) { continue; }

			if (event.type == GroupEvent::Type::FILL) {
				if (event.orderId == group->parentId) { this->growBracket(*group, event.volume); }
				else if (event.orderId == group->bookLegId && !group->resolved) {
					// The take-profit traded: the stop protects that many fewer shares
					auto heldIt = this->held.find(group->heldLegId);
					if (heldIt != this->held.end()) {
						std::shared_ptr<Order> stop = heldIt->second;
						stop->volume -= std::min(stop->volume, event.volume);
						++this->stats.ocoReductions;
						if (stop->volume == 0) { this->cancelHeld(stop, agent); }
					}
					auto bookOrder = agent->activeAsks.count(group->bookLegId) || agent->activeBids.count(group->bookLegId);
					if (!bookOrder) { this->resolveGroup(*group); }   // filled in full
				}
			}
			else {
				// A cancelled child takes its sibling with it. A cancelled entry leaves the children,
				// which protect what it already filled.
				if (group->resolved || event.orderId == group->parentId) { continue; }
				if (event.orderId == group->bookLegId) {
					auto heldIt = this->held.find(group->heldLegId);
					if (heldIt != this->held.end()) { this->cancelHeld(heldIt->second, agent); }
				}
				else if (event.orderId == group->heldLegId) {
					std::shared_ptr<Order> book;
					auto a = agent->activeAsks.find(group->bookLegId);
					if (a != agent->activeAsks.end()) { book = a->second; }
					auto b = agent->activeBids.find(group->bookLegId);
					if (b != agent->activeBids.end()) { book = b->second; }
					if (book != nullptr) { this->OB.cancelOrder(book, agent); }
				}
				this->resolveGroup(*group);
			}
		}
	}
}

void Broker::growBracket(OrderGroup& group, unsigned int volume) {
	if (group.resolved || volume == 0) { return; }
	std::shared_ptr<Agent> agent = this->OB.getAgent(group.agentId);
	if (agent == nullptr) { return; }
	++this->stats.bracketGrowths;

	// The stop-loss first: it shares the take-profit's reservation, so it must exist before
	// the take-profit takes the shares
	auto heldIt = this->held.find(group.heldLegId);
	if (heldIt != this->held.end()) { heldIt->second->volume += volume; }
	else {
		OrderRequest sl{ group.childSide, OrderType::MARKET };
		sl.stopPrice = group.stopLoss;
		sl.volume = volume;
		sl.tif = group.childTif;
		std::shared_ptr<Order> stop = this->holdShared(sl, agent, group.id);
		if (stop == nullptr) { return; }   // the market is already through the stop-loss
		group.heldLegId = stop->id;
	}

	// Then the take-profit: placed the first time, grown by a replace after that
	std::shared_ptr<Order> book;
	auto a = agent->activeAsks.find(group.bookLegId);
	if (a != agent->activeAsks.end()) { book = a->second; }
	auto b = agent->activeBids.find(group.bookLegId);
	if (b != agent->activeBids.end()) { book = b->second; }
	if (book != nullptr) {
		this->replace(book, agent, book->price, book->volume + volume);
	}
	else {
		OrderRequest tp{ group.childSide, OrderType::LIMIT };
		tp.price = group.takeProfit;
		tp.volume = volume;
		tp.tif = group.childTif;
		tp.expiresAtMs = 0.0;
		std::shared_ptr<Order> placed = this->placeTagged(tp, agent, group.id);
		if (placed != nullptr) { group.bookLegId = placed->id; }
	}
}

void Broker::takeOverFromBookLeg(OrderGroup& group, const std::shared_ptr<Order>& heldLeg) {
	std::shared_ptr<Agent> agent = this->OB.getAgent(group.agentId);
	if (agent == nullptr) { return; }
	group.resolved = true;   // before the cancel below raises its event

	std::shared_ptr<Order> book;
	auto a = agent->activeAsks.find(group.bookLegId);
	if (a != agent->activeAsks.end()) { book = a->second; }
	auto b = agent->activeBids.find(group.bookLegId);
	if (b != agent->activeBids.end()) { book = b->second; }
	if (book != nullptr) { this->OB.cancelOrder(book, agent); }

	// A sell stop now holds the shares the limit leg just gave back
	if (heldLeg->reservedByGroup) {
		unsigned int shares = std::min(heldLeg->volume, agent->getTotalHoldings());
		heldLeg->reservedShares = agent->removeHoldings(int(shares));
		heldLeg->volume = shares;
		heldLeg->reservedByGroup = false;
	}
	this->resolveGroup(group);
}

void Broker::resolveGroup(OrderGroup& group) {
	group.resolved = true;
	++this->stats.groupsResolved;
	std::shared_ptr<Agent> agent = this->OB.getAgent(group.agentId);
	if (agent != nullptr && !group.parentId.empty()) {
		auto a = agent->activeAsks.find(group.parentId);
		if (a != agent->activeAsks.end()) { this->OB.cancelOrder(a->second, agent); }
		auto b = agent->activeBids.find(group.parentId);
		if (b != agent->activeBids.end()) { this->OB.cancelOrder(b->second, agent); }
	}
	this->groups.erase(group.id);
}
