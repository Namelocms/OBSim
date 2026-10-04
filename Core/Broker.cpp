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

	// A bid for no more than the account's uncovered short is a buy to cover. It reduces risk,
	// so a broker accepts it whatever the buying power -- in a margin account any shortfall
	// is simply a debit -- which is also what lets a margin call on a short be met at all.
	const bool covering = (request.side == OrderAction::BID) && agent->shortShares > 0
		&& request.volume <= Account::coverableShares(*agent);

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
	order->mark = (request.side == OrderAction::ASK) ? request.mark : SaleMark::LONG;

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

	// Nothing is being watched and nothing has changed: the common case with margin off,
	// which must cost no more than this check
	if (this->OB.dirtyAccounts.empty() && this->triggers.empty() && this->buyIns.empty()) {
		this->OB.printRangeValid = false;
		return;
	}

	int rounds = 0;
	std::unordered_set<std::string> attemptedBuyIn;   // one attempt per account per pump
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

		if (toLiquidate.empty() && toBuyIn.empty()) { break; }

		// 5. Meet the calls and the buy-ins. Their prints and fills feed the next round.
		for (const std::shared_ptr<Agent>& agent : toLiquidate) { this->liquidate(agent); }
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
	if (agent->getTotalHoldings() > 0 || agent->shortShares > 0 || !agent->activeBids.empty() || !agent->activeAsks.empty()) { return; }
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
