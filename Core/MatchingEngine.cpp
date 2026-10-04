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

MatchingEngine::MatchingEngine(OrderBook& ob) : OB(ob) {}

void MatchingEngine::match(std::shared_ptr<Order> order) {
	if (order == nullptr) { return; }

	std::shared_ptr<Agent> agent = this->OB.getAgent(order->agentId);

	// Nothing may fire a trigger while this is raised, see Broker::processTriggers
	++this->OB.matchDepth;
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

	auto it = opposite.begin();
	while (it != opposite.end() && order->volume > 0) {
		std::shared_ptr<Order> resting = *it;

		// A limit order stops at its own price. Checked before anything else, so a walk
		// never reaches past it even to clean up.
		if (isLimit) {
			bool crosses = isBid ? (resting->price <= order->price) : (resting->price >= order->price);
			if (!crosses) { break; }
		}

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

		unsigned int tradeVol = std::min(order->volume, resting->volume);

		// A market bid is the one order with no escrow behind it: it pays as it goes, so
		// each leg is bounded by what the buyer can still afford. Deliberately asymmetric
		// with limit bids, which pre-debit their whole cost -- a market bid reserving
		// against currentPrice truncates a sweep that walks the book (see REFERENCE.md).
		if (isBid && !isLimit) {
			tradeVol = std::min(tradeVol, Account::affordableVolume(*agent, resting->price));
		}
		if (tradeVol < 1) { break; }

		totalCost += this->settleLeg(order, agent, resting, this->OB.getAgent(resting->agentId), tradeVol);

		if (resting->volume == 0) {
			it = opposite.erase(it);
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

	unsigned long long fillable = 0;
	for (const std::shared_ptr<Order>& resting : opposite) {
		if (isLimit) {
			bool crosses = isBid ? (resting->price <= order->price) : (resting->price >= order->price);
			if (!crosses) { break; }
		}
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
		incomingAgent->upsertHolding(Holding(price, volume));
	}
	else {
		// The resting bid's cash was escrowed when it was placed; the incoming ask's
		// shares were reserved when it was made. Only the seller's cash and the buyer's
		// shares are left to move.
		incomingAgent->updateCash(cost);
		restingAgent->upsertHolding(Holding(price, volume));
	}

	this->OB.updateCurrentPrice(price);
	incoming->volume -= volume;
	this->OB.fillOrder(resting, volume);
	this->OB.recordTrade(price, volume, incoming->side);

	// Fees, once per side of every leg: the incoming order took liquidity, the resting one made it
	this->chargeFees(*incoming, *incomingAgent, false, price, volume);
	this->chargeFees(*resting, *restingAgent, true, price, volume);
	// A resting bid that just closed has no further use for what is left of its fee reserve
	if (resting->status == OrderStatus::CLOSED && resting->side == OrderAction::BID) {
		Account::releaseFeeReserve(*restingAgent, *resting);
	}

	// A fill is the only thing that changes a net position, so it is where an opening is seen
	incomingAgent->notePositionChange();
	restingAgent->notePositionChange();

	// Both accounts' margin standing may have moved; the broker re-checks them once matching
	// has returned
	if (this->OB.features.margin.enabled) {
		this->OB.dirtyAccounts.push_back(incomingAgent->id);
		this->OB.dirtyAccounts.push_back(restingAgent->id);
	}

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

void MatchingEngine::chargeFees(Order& order, Agent& agent, bool isMaker, double price, unsigned int volume) {
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
	double exchange = isMaker ? -fees.makerRebate(price) * volume : fees.takerFee(price) * volume;

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
