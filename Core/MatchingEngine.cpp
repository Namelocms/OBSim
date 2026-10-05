#include "include/MatchingEngine.h"
#include "include/Order.h"
#include "include/OrderBook.h"
#include "include/Agent.h"
#include "include/Enums.h"
#include "include/Util.h"
#include "include/SimClock.h"
#include "include/Account.h"
#include "include/FeeSchedule.h"
#include "include/Ledger.h"
#include "include/MarketCalendar.h"
#include <limits>
#include <cmath>

MatchingEngine::MatchingEngine(OrderBook& ob) : OB(ob) {}

void MatchingEngine::match(std::shared_ptr<Order> order) {
	if (order == nullptr) { return; }

	std::shared_ptr<Agent> agent = this->OB.getAgent(order->agentId);

	// Nothing may fire a trigger while this is raised, see Broker::processTriggers
	++this->OB.matchDepth;
	// The midpoint improves on the touch, so resting pegs there trade first
	if (!this->OB.pegBids.empty() || !this->OB.pegAsks.empty()) { this->matchAgainstPegs(order, agent); }
	double totalCost = (order->side == OrderAction::BID)
		? this->sweep(order, agent, this->OB.askQueue)
		: this->sweep(order, agent, this->OB.bidQueue);

	this->finishIncoming(order, agent, totalCost);
	--this->OB.matchDepth;
}

template <typename Queue>
double MatchingEngine::sweep(const std::shared_ptr<Order>& order, const std::shared_ptr<Agent>& agent, Queue& opposite) {
	const bool isBid = (order->side == OrderAction::BID);
	const bool isLimit = (order->type == OrderType::LIMIT);
	const Session session = this->OB.session;
	double totalCost = 0.00;

	// An order outside its sessions does not match at all. A limit order rests until its
	// session comes round; anything that may not rest is cancelled by finishIncoming.
	if (!order->eligibleIn(session)) { return totalCost; }
	// Nor does anything during a LULD trading pause: orders rest for the reopening cross
	if (this->OB.luld.paused) { return totalCost; }
	// And nothing prints outside the LULD bands while they are in force
	const double upperBand = this->OB.luld.active ? this->OB.luld.upper : 0.0;
	const double lowerBand = this->OB.luld.active ? this->OB.luld.lower : 0.0;

	auto it = opposite.begin();
	while (it != opposite.end() && order->volume > 0) {
		std::shared_ptr<Order> resting = *it;

		// A limit order stops at its own price. Checked before anything else, so a walk
		// never reaches past it even to clean up.
		if (isLimit) {
			bool crosses = isBid ? (resting->price <= order->price) : (resting->price >= order->price);
			if (!crosses) { break; }
		}
		if (isBid && upperBand > 0.0 && resting->price > upperBand + 1e-9) { break; }
		if (!isBid && lowerBand > 0.0 && resting->price < lowerBand - 1e-9) { break; }

		// Clean stagnant canceled orders
		if (resting->status == OrderStatus::CANCELED) {
			it = opposite.erase(it);
			continue;
		}

		// A resting order outside its sessions stays in the book and is stepped over, so the
		// book can legitimately sit crossed until its session (or the opening cross) arrives
		if (!resting->eligibleIn(session)) {
			++it;
			continue;
		}

		// Self trade protection cancels the INCOMING order, never the resting one
		if (resting->agentId == order->agentId) {
			order->status = OrderStatus::CANCELED;
			break;
		}

		// A reserve order trades its displayed tip, then refreshes from its reserve below
		unsigned int restingAvailable = (resting->displayQty > 0) ? std::min(resting->tipRemaining, resting->volume) : resting->volume;
		unsigned int tradeVol = std::min(order->volume, restingAvailable);

		// A market bid is the one order with no escrow behind it: it pays as it goes, so
		// each leg is bounded by what the buyer can still afford. Deliberately asymmetric
		// with limit bids, which pre-debit their whole cost -- a market bid reserving
		// against currentPrice truncates a sweep that walks the book (see REFERENCE.md).
		if (isBid && !isLimit) {
			// ...except a buy to cover, which a broker accepts whatever the buying power: in a
			// margin account any shortfall is just a debit. Without this a margin call on a
			// short could never be met with a market order.
			unsigned int cap = Account::affordableVolume(*agent, resting->price);
			if (agent->shortShares > 0) { cap = std::max(cap, agent->shortShares); }
			tradeVol = std::min(tradeVol, cap);
		}
		if (tradeVol < 1) { break; }

		totalCost += this->settleLeg(order, agent, resting, this->OB.getAgent(resting->agentId), tradeVol);
		if (resting->displayQty > 0) { resting->tipRemaining -= std::min(resting->tipRemaining, tradeVol); }

		if (resting->volume == 0) {
			it = opposite.erase(it);
		}
		// The tip ran out with reserve behind it: a fresh tip goes to the back of its price
		// level with a new timestamp, and the walk carries on to whoever is next
		else if (resting->displayQty > 0 && resting->tipRemaining == 0) {
			it = opposite.erase(it);
			resting->timestamp = this->OB.clock->simTimeMs;
			resting->tipRemaining = std::min(resting->displayQty, resting->volume);
			auto refreshed = opposite.insert(resting).first;
			// If nothing else waits at that price the refreshed tip sorts ahead of where the walk
			// would go next, so step back onto it: the incoming order keeps trading the reserve
			if (it == opposite.end() || opposite.key_comp()(*refreshed, *it)) { it = refreshed; }
			continue;
		}
		// The resting order is still there, so the incoming one is exhausted (or, for a
		// market bid, out of cash). Either way the walk is over.
		else { break; }
	}

	return totalCost;
}

unsigned int MatchingEngine::fillableVolume(const std::shared_ptr<Order>& order) const {
	if (order == nullptr) { return 0; }
	return (order->side == OrderAction::BID)
		? this->fillableFrom(order, this->OB.askQueue)
		: this->fillableFrom(order, this->OB.bidQueue);
}

template <typename Queue>
unsigned int MatchingEngine::fillableFrom(const std::shared_ptr<Order>& order, const Queue& opposite) const {
	const bool isBid = (order->side == OrderAction::BID);
	const bool isLimit = (order->type == OrderType::LIMIT);
	const Session session = this->OB.session;
	if (!order->eligibleIn(session)) { return 0; }
	if (this->OB.luld.paused) { return 0; }
	const double upperBand = this->OB.luld.active ? this->OB.luld.upper : 0.0;
	const double lowerBand = this->OB.luld.active ? this->OB.luld.lower : 0.0;

	unsigned long long fillable = 0;
	for (const std::shared_ptr<Order>& resting : opposite) {
		if (isLimit) {
			bool crosses = isBid ? (resting->price <= order->price) : (resting->price >= order->price);
			if (!crosses) { break; }
		}
		if (isBid && upperBand > 0.0 && resting->price > upperBand + 1e-9) { break; }
		if (!isBid && lowerBand > 0.0 && resting->price < lowerBand - 1e-9) { break; }
		if (resting->status == OrderStatus::CANCELED) { continue; }
		if (!resting->eligibleIn(session)) { continue; }
		if (resting->agentId == order->agentId) { break; } // match would be killed here
		fillable += resting->volume;
		if (fillable >= order->volume) { return order->volume; }
	}
	return (unsigned int)fillable;
}

double MatchingEngine::settleLeg(const std::shared_ptr<Order>& incoming, const std::shared_ptr<Agent>& incomingAgent,
	const std::shared_ptr<Order>& resting, const std::shared_ptr<Agent>& restingAgent, unsigned int volume) {

	// Every trade happens at the resting order's price
	double price = resting->price;
	double cost = roundTo(volume * price);

	if (incoming->side == OrderAction::BID) {
		if (incoming->type == OrderType::LIMIT) {
			// The bid's whole cost was escrowed at its own price. Refund the buyer for
			// any price improvement, the rest of the escrow pays the seller.
			double refund = roundTo(volume * (incoming->price - price));
			if (refund > 0) { incomingAgent->updateCash(refund); }
		}
		else {
			// No escrow, a market bid pays as it goes
			incomingAgent->updateCash(-cost);
		}
		restingAgent->updateCash(cost);
		this->deliverShares(*incomingAgent, price, volume);
		if (resting->isShortSale()) { this->openShort(*resting, *restingAgent, volume); }
	}
	else {
		// The resting bid's cash was escrowed when it was placed; the incoming ask's
		// shares were reserved when it was made -- or, for a short sale, are owed from now.
		// Only the seller's cash and the buyer's shares are left to move.
		incomingAgent->updateCash(cost);
		this->deliverShares(*restingAgent, price, volume);
		if (incoming->isShortSale()) { this->openShort(*incoming, *incomingAgent, volume); }
	}

	this->OB.updateCurrentPrice(price);
	incoming->volume -= volume;
	this->OB.fillOrder(resting, volume);
	this->OB.recordTrade(price, volume, incoming->side);

	this->afterFill(*incoming, *incomingAgent, *resting, *restingAgent, price, volume, false);

	return cost;
}

void MatchingEngine::finishIncoming(const std::shared_ptr<Order>& order, const std::shared_ptr<Agent>& agent, double totalCost) {
	const bool isBid = (order->side == OrderAction::BID);

	// IOC and FOK never rest. Anything left of one is cancelled, its escrow returned, exactly
	// as for a limit order self trade protection killed.
	const bool mayRest = (order->tif != TimeInForce::IOC && order->tif != TimeInForce::FOK);
	if (order->type == OrderType::LIMIT && !mayRest && order->volume > 0) {
		order->status = OrderStatus::CANCELED;
	}

	if (order->type == OrderType::LIMIT) {
		// Self trade protection (or an IOC/FOK remainder) killed this order before it could rest. Return the escrow
		// and keep it out of the book, it must not be counted or left behind.
		if (order->status == OrderStatus::CANCELED) {
			if (order->mark == SaleMark::SHORT) { this->OB.lending.releaseLocate(order->volume); }
			if (isBid) {
				if (order->volume > 0) { agent->updateCash(order->price * order->volume); }
				Account::releaseFeeReserve(*agent, *order);
			}
			else {
				std::vector<Holding> returnableShares = order->getReturnableShares();
				for (Holding h : returnableShares) { agent->upsertHolding(h); }
			}
			return;
		}

		if (order->volume > 0) {
			agent->upsertActiveOrder(order);
			this->OB.addOrder(order);
		}
		else {
			order->status = OrderStatus::CLOSED;
			if (isBid) { Account::releaseFeeReserve(*agent, *order); }
		}
		return;
	}

	// Market orders never rest. Whatever is left is cancelled, and an ask's unsold
	// reserved shares go back to the seller.
	if (order->volume > 0) {
		order->status = OrderStatus::CANCELED;
		if (!isBid) {
			std::vector<Holding> returnableShares = order->getReturnableShares();
			for (Holding h : returnableShares) { agent->upsertHolding(h); }
			if (order->mark == SaleMark::SHORT) { this->OB.lending.releaseLocate(order->volume); }
		}
	}
	else {
		order->status = OrderStatus::CLOSED;
	}

	// Volume Weighted Average Price (VWAP) of the shares traded for the order
	unsigned int totalVolume = order->entryVolume - order->volume;
	if (totalVolume > 0) {
		order->price = totalCost / totalVolume;
	}
}

void MatchingEngine::chargeFees(Order& order, Agent& agent, bool isMaker, double price, unsigned int volume, bool auction) {
	if (!Account::feesEnabled(agent)) { return; }
	const FeeSchedule& fees = Account::feeSchedule(agent);
	Ledger& ledger = this->OB.ledger;
	const bool isSell = (order.side == OrderAction::ASK);

	order.filledVolume += volume;
	order.filledValue += price * volume;

	// Commission is defined on the order as a whole, so charge only what this leg adds to it
	double commissionTotal = fees.commission(order.filledVolume, order.filledValue);
	double commission = commissionTotal - order.commissionAccrued;
	order.commissionAccrued = commissionTotal;

	// Maker-taker is per share, per leg. A rebate is a negative fee.
	// A cross has no maker or taker; its fee schedule is the exchange's own and is left at zero
	double exchange = auction ? 0.0 : (isMaker ? -fees.makerRebate(price) * volume : fees.takerFee(price) * volume);

	// Regulators charge the seller only
	double regulatory = 0.0;
	if (isSell) {
		double tafTotal = fees.taf(order.filledVolume);
		regulatory = fees.secFeeRate * price * volume + (tafTotal - order.tafAccrued);
		order.tafAccrued = tafTotal;
	}

	double accrued = commission + exchange + regulatory;
	order.feeAccrued += accrued;

	// Cash moves in whole cents: charge the increase in the order's rounded total
	double charge = roundTo(order.feeAccrued) - order.feeCharged;
	order.feeCharged += charge;

	ledger.commissions += commission;
	ledger.exchangeFees += exchange;
	ledger.regulatoryFees += regulatory;
	ledger.feeRounding += charge - accrued;

	// A bid pays from the reserve it escrowed for exactly this; anything beyond, and any
	// rebate coming back, goes through cash
	if (charge > 0.0 && order.feeReserve > 0.0) {
		double fromReserve = std::min(charge, order.feeReserve);
		order.feeReserve = roundTo(order.feeReserve - fromReserve);
		charge = roundTo(charge - fromReserve);
	}
	if (charge != 0.0) { agent.updateCash(-charge); }
}

void MatchingEngine::deliverShares(Agent& buyer, double price, unsigned int volume) {
	unsigned int cover = std::min(volume, buyer.shortShares);
	if (cover > 0) {
		// The unborrowed part of a short is the regulatory problem, so it is closed first
		unsigned int fails = buyer.shortShares - buyer.borrowedShares;
		unsigned int fromFails = std::min(cover, fails);
		buyer.shortShares -= cover;
		this->OB.lending.returnLoan(buyer, cover - fromFails);
		buyer.buyInDue -= std::min(buyer.buyInDue, cover);
	}
	if (volume > cover) { buyer.upsertHolding(Holding(price, volume - cover)); }
}

void MatchingEngine::openShort(const Order& sale, Agent& seller, unsigned int volume) {
	seller.shortShares += volume;
	this->OB.lending.shortSoldBy[int(seller.type)][int(seller.subType)] += volume;
	if (sale.mark == SaleMark::SHORT) { this->OB.lending.openLoan(seller, volume, this->OB.clock->simTimeMs); }
	// SHORT_EXEMPT: owed but not borrowed, a fail until Broker::closeOutFails deals with it
}

void MatchingEngine::afterFill(Order& taker, Agent& takerAgent, Order& maker, Agent& makerAgent, double price,
	unsigned int volume, bool auction) {
	// A fill on an OCO or bracket member is for the broker to act on once matching is done
	if (!taker.groupId.empty()) { this->OB.groupEvents.push_back({ GroupEvent::Type::FILL, taker.groupId, taker.id, volume }); }
	if (!maker.groupId.empty()) { this->OB.groupEvents.push_back({ GroupEvent::Type::FILL, maker.groupId, maker.id, volume }); }

	// Fees, once per side of every leg: the taker took liquidity, the maker made it
	this->chargeFees(taker, takerAgent, false, price, volume, auction);
	this->chargeFees(maker, makerAgent, true, price, volume, auction);
	// A bid that just closed has no further use for what is left of its fee reserve
	if (maker.status == OrderStatus::CLOSED && maker.side == OrderAction::BID) { Account::releaseFeeReserve(makerAgent, maker); }
	if (auction && taker.volume == 0 && taker.side == OrderAction::BID) { Account::releaseFeeReserve(takerAgent, taker); }

	// A fill is the only thing that changes a net position, so it is where an opening is seen
	takerAgent.notePositionChange();
	makerAgent.notePositionChange();

	// Both accounts' margin standing may have moved; the broker re-checks them once matching
	// has returned
	if (this->OB.features.margin.enabled) {
		this->OB.dirtyAccounts.push_back(takerAgent.id);
		this->OB.dirtyAccounts.push_back(makerAgent.id);
	}
	// And what each can lend has changed with what it holds
	if (Account::shortingEnabled(takerAgent)) {
		this->OB.lending.refreshLender(takerAgent);
		this->OB.lending.refreshLender(makerAgent);
	}
}

// ---- Auctions (OrderModelPlan Step 3.1) ----

namespace {

/* One order's part in a cross */
struct CrossEntry {
	std::shared_ptr<Order> order;
	/* The price it will trade at worst: a bid's limit (+inf for a market sell... no: a market
	*  BUY is bounded by its collar escrow), an ask's limit (0 for a market sell) */
	double limit;
	bool market;
};

}

static void gatherCross(const OrderBook& ob, TimeInForce which, std::vector<CrossEntry>& bids, std::vector<CrossEntry>& asks) {
	for (const std::shared_ptr<Order>& o : ob.auctionOrders) {
		if (o->status != OrderStatus::OPEN || o->tif != which || o->volume == 0) { continue; }
		bool market = (o->type == OrderType::MARKET);
		if (o->side == OrderAction::BID) { bids.push_back({ o, o->price, market }); }   // a market buy's price is its collar
		else { asks.push_back({ o, market ? 0.0 : o->price, market }); }
	}
	// The continuous book takes part, as it does in the real crosses: every resting order
	// that may trade in the regular session
	for (const std::shared_ptr<Order>& o : ob.bidQueue) {
		if (o->status == OrderStatus::OPEN && o->volume > 0 && o->eligibleIn(Session::REGULAR)) { bids.push_back({ o, o->price, false }); }
	}
	for (const std::shared_ptr<Order>& o : ob.askQueue) {
		if (o->status == OrderStatus::OPEN && o->volume > 0 && o->eligibleIn(Session::REGULAR)) { asks.push_back({ o, o->price, false }); }
	}
}

CrossResult MatchingEngine::indicativeCross(TimeInForce which, double reference) const {
	CrossResult best;
	std::vector<CrossEntry> bids, asks;
	gatherCross(this->OB, which, bids, asks);
	if (bids.empty() || asks.empty()) { return best; }

	std::vector<double> candidates;
	for (const CrossEntry& e : bids) { candidates.push_back(e.limit); }
	for (const CrossEntry& e : asks) { if (!e.market) { candidates.push_back(e.limit); } }
	if (reference > 0.0) { candidates.push_back(reference); }
	std::sort(candidates.begin(), candidates.end());
	candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

	bool found = false;
	for (double p : candidates) {
		if (!(p > 0.0)) { continue; }
		unsigned long long buy = 0, sell = 0;
		for (const CrossEntry& e : bids) { if (e.limit >= p - 1e-12) { buy += e.order->volume; } }
		for (const CrossEntry& e : asks) { if (e.limit <= p + 1e-12) { sell += e.order->volume; } }
		unsigned long long matched = std::min(buy, sell);
		unsigned long long imbalance = (buy > sell) ? buy - sell : sell - buy;
		bool better = !found
			|| matched > best.matched
			|| (matched == best.matched && imbalance < best.imbalance)
			|| (matched == best.matched && imbalance == best.imbalance
				&& std::fabs(p - reference) < std::fabs(best.price - reference) - 1e-12);
		if (better) {
			found = true;
			best.price = p;
			best.matched = matched;
			best.buyVolume = buy;
			best.sellVolume = sell;
			best.imbalance = imbalance;
			best.imbalanceSide = (buy >= sell) ? OrderAction::BID : OrderAction::ASK;
		}
	}
	if (best.matched == 0) { best = CrossResult(); }
	return best;
}

void MatchingEngine::settleAuctionLeg(Order& bid, Agent& bidAgent, Order& ask, Agent& askAgent, double price, unsigned int volume) {
	double cost = roundTo(volume * price);

	// Every bid in a cross was escrowed at its limit (a market buy at its collar), so each gets
	// back the difference to the single cross price -- including a bid that was resting in the
	// book, which continuous trading never has to do
	double refund = roundTo(volume * (bid.price - price));
	if (refund > 0) { bidAgent.updateCash(refund); }
	askAgent.updateCash(cost);
	this->deliverShares(bidAgent, price, volume);
	if (ask.isShortSale()) { this->openShort(ask, askAgent, volume); }

	// Each side's own volume moves here; the book's counters are kept by fillOrder for book
	// orders, and auction orders are not in them
	auto fill = [this](Order& o, Agent& agent, unsigned int v) {
		if (o.inAuction) {
			o.volume -= v;
			if (o.volume == 0) { o.status = OrderStatus::CLOSED; }
		}
		else {
			std::shared_ptr<Order> self = nullptr;
			const auto& mine = (o.side == OrderAction::BID) ? agent.activeBids : agent.activeAsks;
			auto it = mine.find(o.id);
			if (it != mine.end()) { self = it->second; }
			if (self != nullptr) { this->OB.fillOrder(self, v); }
			else { o.volume -= v; }
		}
	};
	fill(bid, bidAgent, volume);
	fill(ask, askAgent, volume);

	// No maker or taker in a cross; afterFill only needs the two sides
	this->afterFill(bid, bidAgent, ask, askAgent, price, volume, true);
}

CrossResult MatchingEngine::runCross(TimeInForce which, PrintKind kind, double reference) {
	CrossResult result = this->indicativeCross(which, reference);
	++this->OB.matchDepth;

	if (result.matched > 0) {
		std::vector<CrossEntry> bids, asks;
		gatherCross(this->OB, which, bids, asks);
		double p = result.price;

		// Who trades: everything priced at or through the cross price. Market orders first, then
		// by price, then by time, then by id
		auto eligibleBid = [p](const CrossEntry& e) { return e.limit >= p - 1e-12; };
		auto eligibleAsk = [p](const CrossEntry& e) { return e.limit <= p + 1e-12; };
		bids.erase(std::remove_if(bids.begin(), bids.end(), [&](const CrossEntry& e) { return !eligibleBid(e); }), bids.end());
		asks.erase(std::remove_if(asks.begin(), asks.end(), [&](const CrossEntry& e) { return !eligibleAsk(e); }), asks.end());
		auto priority = [](bool bidSide) {
			return [bidSide](const CrossEntry& a, const CrossEntry& b) {
				if (a.market != b.market) { return a.market; }
				if (a.limit != b.limit) { return bidSide ? a.limit > b.limit : a.limit < b.limit; }
				if (a.order->timestamp != b.order->timestamp) { return a.order->timestamp < b.order->timestamp; }
				return a.order->id < b.order->id;
			};
		};
		std::sort(bids.begin(), bids.end(), priority(true));
		std::sort(asks.begin(), asks.end(), priority(false));

		// Pair them off in priority order. An agent never trades with itself: a pair that would
		// is stepped over, and the bid looks for the next seller.
		unsigned long long remaining = result.matched, traded = 0;
		for (CrossEntry& b : bids) {
			if (remaining == 0) { break; }
			std::shared_ptr<Agent> bidAgent = this->OB.getAgent(b.order->agentId);
			if (bidAgent == nullptr) { continue; }
			for (CrossEntry& a : asks) {
				if (remaining == 0 || b.order->volume == 0) { break; }
				if (a.order->volume == 0 || a.order->agentId == b.order->agentId) { continue; }
				std::shared_ptr<Agent> askAgent = this->OB.getAgent(a.order->agentId);
				if (askAgent == nullptr) { continue; }
				unsigned int v = (unsigned int)std::min<unsigned long long>({ b.order->volume, a.order->volume, remaining });
				this->settleAuctionLeg(*b.order, *bidAgent, *a.order, *askAgent, p, v);
				remaining -= v;
				traded += v;
			}
		}

		// Book orders the cross filled in full leave their queues
		for (auto it = this->OB.bidQueue.begin(); it != this->OB.bidQueue.end();) {
			if ((*it)->status == OrderStatus::CLOSED) { it = this->OB.bidQueue.erase(it); } else { ++it; }
		}
		for (auto it = this->OB.askQueue.begin(); it != this->OB.askQueue.end();) {
			if ((*it)->status == OrderStatus::CLOSED) { it = this->OB.askQueue.erase(it); } else { ++it; }
		}

		// One print for the whole cross, at its one price
		if (traded > 0) {
			this->OB.updateCurrentPrice(p);
			this->OB.recordTrade(p, (unsigned int)traded, result.imbalanceSide, kind);
		}
		result.matched = traded;
	}

	// Whatever on-open or on-close orders the cross did not fill are cancelled now
	std::vector<std::shared_ptr<Order>> leftover;
	for (const std::shared_ptr<Order>& o : this->OB.auctionOrders) { if (o->tif == which) { leftover.push_back(o); } }
	for (const std::shared_ptr<Order>& o : leftover) {
		std::shared_ptr<Agent> agent = this->OB.getAgent(o->agentId);
		if (o->status == OrderStatus::CLOSED) {
			this->OB.removeFromAuction(o);
			if (agent != nullptr) { agent->removeActiveOrder(o); Account::releaseFeeReserve(*agent, *o); }
		}
		else if (agent != nullptr) { this->OB.cancelOrder(o, agent); }
		else { this->OB.removeFromAuction(o); }
	}

	--this->OB.matchDepth;
	return result;
}

// ---- Midpoint pegs (OrderModelPlan Step 3.4) ----

void MatchingEngine::settlePegLeg(const std::shared_ptr<Order>& incoming, const std::shared_ptr<Agent>& incomingAgent,
	const std::shared_ptr<Order>& peg, const std::shared_ptr<Agent>& pegAgent, double mid, unsigned int volume) {
	double cost = roundTo(volume * mid);
	const bool incomingBuys = (incoming->side == OrderAction::BID);
	const std::shared_ptr<Order>& bid = incomingBuys ? incoming : peg;
	const std::shared_ptr<Agent>& buyer = incomingBuys ? incomingAgent : pegAgent;
	const std::shared_ptr<Order>& ask = incomingBuys ? peg : incoming;
	const std::shared_ptr<Agent>& seller = incomingBuys ? pegAgent : incomingAgent;

	// A limit or pegged bid was escrowed at its price and gets the difference back; a market
	// bid pays as it goes
	if (bid->type == OrderType::LIMIT) {
		double refund = roundTo(volume * (bid->price - mid));
		if (refund > 0) { buyer->updateCash(refund); }
	}
	else { buyer->updateCash(-cost); }
	seller->updateCash(cost);
	this->deliverShares(*buyer, mid, volume);
	if (ask->isShortSale()) { this->openShort(*ask, *seller, volume); }

	incoming->volume -= volume;
	peg->volume -= volume;
	if (peg->volume == 0) {
		peg->status = OrderStatus::CLOSED;
		auto& pegs = (peg->side == OrderAction::BID) ? this->OB.pegBids : this->OB.pegAsks;
		pegs.erase(std::remove(pegs.begin(), pegs.end(), peg), pegs.end());
		peg->inPegBook = false;
		pegAgent->removeActiveOrder(peg);
	}

	// A midpoint print is a real trade at a price that may sit between ticks
	this->OB.updateCurrentPrice(mid);
	this->OB.recordTrade(mid, volume, incoming->side);
	this->afterFill(*incoming, *incomingAgent, *peg, *pegAgent, mid, volume, false);
}

void MatchingEngine::matchAgainstPegs(const std::shared_ptr<Order>& incoming, const std::shared_ptr<Agent>& agent) {
	if (agent == nullptr || incoming->volume == 0) { return; }
	if (!incoming->eligibleIn(this->OB.session) || this->OB.luld.paused) { return; }
	double mid = this->OB.midpoint();
	if (!(mid > 0.0)) { return; }

	const bool buys = (incoming->side == OrderAction::BID);
	// The incoming order has to reach the midpoint; a market order always does
	if (incoming->type == OrderType::LIMIT && (buys ? incoming->price < mid - 1e-12 : incoming->price > mid + 1e-12)) { return; }

	auto& pegs = buys ? this->OB.pegAsks : this->OB.pegBids;
	std::vector<std::shared_ptr<Order>> queue = pegs;   // a copy: settling removes filled pegs
	for (const std::shared_ptr<Order>& peg : queue) {
		if (incoming->volume == 0) { break; }
		if (peg->status != OrderStatus::OPEN || peg->agentId == incoming->agentId) { continue; }
		// A peg trades at the midpoint only if its own cap allows it
		if (buys ? peg->price > mid + 1e-12 : peg->price < mid - 1e-12) { continue; }
		if (!peg->eligibleIn(this->OB.session)) { continue; }
		std::shared_ptr<Agent> pegAgent = this->OB.getAgent(peg->agentId);
		if (pegAgent == nullptr) { continue; }
		unsigned int v = std::min(incoming->volume, peg->volume);
		if (buys && incoming->type == OrderType::MARKET) {
			unsigned int cap = Account::affordableVolume(*agent, mid);
			if (agent->shortShares > 0) { cap = std::max(cap, agent->shortShares); }
			v = std::min(v, cap);
		}
		if (v == 0) { break; }
		this->settlePegLeg(incoming, agent, peg, pegAgent, mid, v);
	}
}

void MatchingEngine::matchPeg(std::shared_ptr<Order> order) {
	if (order == nullptr) { return; }
	std::shared_ptr<Agent> agent = this->OB.getAgent(order->agentId);
	if (agent == nullptr) { return; }

	++this->OB.matchDepth;
	// An arriving peg first meets the opposite pegs at the midpoint, within both caps
	this->matchAgainstPegs(order, agent);
	--this->OB.matchDepth;

	if (order->volume == 0) {
		order->status = OrderStatus::CLOSED;
		if (order->side == OrderAction::BID) { Account::releaseFeeReserve(*agent, *order); }
		return;
	}
	// IOC and FOK pegs do not rest
	if (order->tif == TimeInForce::IOC || order->tif == TimeInForce::FOK) {
		order->status = OrderStatus::CANCELED;
		if (order->side == OrderAction::BID) {
			agent->updateCash(order->price * order->volume);
			Account::releaseFeeReserve(*agent, *order);
		}
		else {
			for (Holding h : order->getReturnableShares()) { agent->upsertHolding(h); }
			if (order->mark == SaleMark::SHORT) { this->OB.lending.releaseLocate(order->volume); }
		}
		return;
	}
	order->inPegBook = true;
	(order->side == OrderAction::BID ? this->OB.pegBids : this->OB.pegAsks).push_back(order);
	agent->upsertActiveOrder(order);
}
