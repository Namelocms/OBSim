#include "include/Agent.h"
#include "include/Enums.h"
#include "include/Util.h"
#include "include/Order.h"
#include "include/Holding.h"
#include "include/OrderBook.h"
#include "include/MatchingEngine.h"
#include "include/SimClock.h"
#include "include/MarketCalendar.h"

Agent::Agent(std::string id, double reactionTimeFloor, double cash, AgentStatus status, AgentType type, AgentSubType subType, OrderBook& ob, MatchingEngine& me) :
	id(id), 
	reactionTimeFloor(reactionTimeFloor), 
	reactionTime(reactionTimeFloor), cash(cash), 
	status(status), type(type), subType(subType), 
	OB(ob), ME(me), 
	sentiment(randomDouble(-0.9, 0.9)), sentimentTemperature(0.5),
	participationThreshold(randomDouble(0.0, 1.0)) {

	this->sentimentActions = { OrderAction::ASK, OrderAction::HOLD, OrderAction::BID };

	this->rollSentimentProcess();

	// Seed the average with the opening sentiment. Assigned here rather than in the
	// initialiser list because members initialise in declaration order, not list order,
	// so reading sentiment there would depend on where it happens to be declared.
	// Consumes no RNG, the value is already drawn.
	this->sentimentEwma = this->sentiment;
}

// ---- Cash Operations ----

void Agent::updateCash(double amt) {
	this->cash = roundTo(this->cash + amt, CASH_PRECISION);
}

// ---- Holdings Operations ----

void Agent::upsertHolding(Holding holding) {
	// Insert new holding if price key does not already exist
	auto attempt = this->holdings.try_emplace(holding.price, holding);
	
	auto it = attempt.first;
	auto inserted = attempt.second;

	// Not inserted so price key existed, just add volume
	if (!inserted) {
		it->second.volume += holding.volume;
	}
}
std::vector<Holding> Agent::removeHoldings(int volume) {
	std::vector<Holding> removedHoldings;

	while (volume > 0 && !this->holdings.empty()) {
		auto it = this->holdings.begin();
		Holding& currentHolding = it->second;

		// Clean faulty holdings if there are any
		if (currentHolding.volume <= 0) {
			this->holdings.erase(it);
			continue;
		}

		if (currentHolding.volume <= volume) {
			// Fully remove holding
			volume -= currentHolding.volume;
			removedHoldings.push_back(std::move(currentHolding));
			this->holdings.erase(it);
		}
		else {
			removedHoldings.emplace_back(currentHolding.price, volume);
			currentHolding.volume -= volume;
			volume = 0;
		}
	}

	return removedHoldings;
}
unsigned int Agent::getTotalHoldings() {
	return std::accumulate(this->holdings.begin(), this->holdings.end(), 0,
		[](unsigned int sum, const auto& kv) {
			return sum + kv.second.volume;
		});
}

// ---- Active Order Operations ----

void Agent::upsertActiveOrder(std::shared_ptr<Order> order) {
	switch (order->side) {
	case OrderAction::BID:
		this->activeBids.insert_or_assign(order->id, std::move(order));
		break;
	case OrderAction::ASK:
		this->activeAsks.insert_or_assign(order->id, std::move(order));
		break;
	}
}
void Agent::removeActiveOrder(std::shared_ptr<Order> order) {
	bool orderRemoved = false;

	switch (order->side) {
	case OrderAction::BID:
		orderRemoved = this->activeBids.erase(order->id);
		break;
	case OrderAction::ASK:
		orderRemoved = this->activeAsks.erase(order->id);
		break;
	}

	if (orderRemoved) { /* Order was removed */ }
	else { /* Order ID does not exist */ }
}

// ---- Action Operations ----

void Agent::actRandom() {
	++this->actionCount;

	// An agent on its way out has no opinion left to act on, only a position to close
	if (this->status == AgentStatus::LEAVING) { this->actFlatten(); return; }

	OrderAction action = this->getRandomAction();
	OrderType orderType = randomInt(0, 1) ? OrderType::MARKET : OrderType::LIMIT;
	std::shared_ptr<Order> order;

	switch (action) {
	case OrderAction::BID:
		switch (orderType) {
		case OrderType::MARKET:
			if (this->OB.session != Session::REGULAR) { break; } // No market orders outside regular market hours
			// Institutions and algos quote passively, real desks use marketable limits
			// to cap slippage rather than sending naked market orders
			if (this->type == AgentType::INSTITUTION || this->subType == AgentSubType::ALGO) { break; }
			order = this->makeMarketBid();
			// An agent that cannot afford or source the order simply does nothing
			if (order == nullptr) { break; }
			this->ME.matchMarketBid(order);
			break;
		case OrderType::LIMIT:
			if (this->subType == AgentSubType::ALGO && this->activeBids.size() > 0) { break; }
			order = this->makeLimitBid();
			if (order == nullptr) { break; }
			this->ME.matchLimitBid(order);
			break;
		}
		break;
	case OrderAction::ASK:
		switch (orderType) {
		case OrderType::MARKET:
			if (this->OB.session != Session::REGULAR) { break; }
			if (this->type == AgentType::INSTITUTION || this->subType == AgentSubType::ALGO) { break; }
			order = this->makeMarketAsk();
			if (order == nullptr) { break; }
			this->ME.matchMarketAsk(order);
			break;
		case OrderType::LIMIT:
			if (this->subType == AgentSubType::ALGO && this->activeAsks.size() > 0) { break; }
			order = this->makeLimitAsk();
			if (order == nullptr) { break; }
			this->ME.matchLimitAsk(order);
			break;
		}
		break;
	case OrderAction::CANCEL:
		this->cancelOrder();
		break;
	case OrderAction::HOLD:
		this->hold();
		break;
	}
}
void Agent::actFlatten() {
	// Keep the running average moving even while unwinding, so a slot's final adversity is
	// still meaningful to anything measuring why it left
	this->updateSentiment();

	OrderAction side = this->flattenSide();

	// Replace the previous attempt rather than posting beside it. A partially filled unwind
	// order leaves a resting remainder, and adding another would split the position across
	// stale quotes so the slot could never come clean. Collect first: cancelOrder erases
	// from the very map being iterated.
	const auto& resting = (side == OrderAction::BID) ? this->activeBids : this->activeAsks;
	std::vector<std::shared_ptr<Order>> stale;
	stale.reserve(resting.size());
	for (const auto& kv : resting) { stale.push_back(kv.second); }
	for (const std::shared_ptr<Order>& o : stale) { this->OB.cancelOrder(o, shared_from_this()); }

	std::shared_ptr<Order> order;

	// Always crosses, always for the whole remaining position. Marketable limits work in
	// every session, so unwinding does not stall outside REGULAR the way a market order would.
	if (side == OrderAction::ASK) {
		if (this->getTotalHoldings() < 1) { return; }
		order = this->makeLimitAsk(true, true);
		if (order == nullptr) { return; }
		this->ME.matchLimitAsk(order);
	}
	else {
		order = this->makeLimitBid(true, true);
		if (order == nullptr) { return; }
		this->ME.matchLimitBid(order);
	}
}
OrderAction Agent::entrySide() const {
	return (this->directionalBias >= 0.0) ? OrderAction::BID : OrderAction::ASK;
}
OrderAction Agent::flattenSide() const {
	return (this->directionalBias >= 0.0) ? OrderAction::ASK : OrderAction::BID;
}
bool Agent::shouldAct(Session session, double simTimeMs) const {
	// A pooled slot is not an agent right now, it is an empty seat
	if (this->status == AgentStatus::POOLED) { return false; }

	// Gating is suspended while leaving. Without this an agent that went dormant off hours
	// could never work its position off, and its slot could never be reused.
	if (this->status == AgentStatus::LEAVING) { return true; }

	return this->isParticipating(session, simTimeMs);
}
bool Agent::isStranded(double nowMs) const {
	if (this->status != AgentStatus::LEAVING || this->leavingSinceMs <= 0.0) { return false; }
	return (nowMs - this->leavingSinceMs) >= (TRANSIENT_LEAVING_GRACE_MINUTES * 60'000.0);
}
void Agent::rollSentimentProcess() {
	// ---- Conviction spread ----
	// The same two draws the model has always made, combined the same way. sigma /
	// sqrt(2 * theta) IS the stationary standard deviation of an OU process, which is what
	// the old code's noiseScaling term collapsed to once exp(-2*theta*r) reached zero -- so
	// this reproduces today's spread of convictions exactly, agent for agent.
	double shapeTheta = randomDouble(0.01, 1.0);
	double shapeSigma = randomDouble(0.0, 0.99);
	this->sentimentStationarySd = shapeSigma / std::sqrt(2.0 * shapeTheta);

	// ---- Persistence ----
	// Drawn as a rate, so the distribution keeps the shape of the uniform draw that used to
	// feed sentimentTheta directly, then converted into a duration in exactly one place.
	double halfLifeMinutes = 1.0 / randomDouble(1.0 / SENTIMENT_HALFLIFE_MAX_MINUTES,
		1.0 / SENTIMENT_HALFLIFE_MIN_MINUTES);
	this->sentimentHalfLifeMs = halfLifeMinutes * 60'000.0;

	// theta in per-millisecond units, matching the r that updateSentiment passes it
	this->sentimentTheta = 0.693147180559945309417 / this->sentimentHalfLifeMs;
	// and sigma chosen so the stationary spread above is what the process actually settles at
	this->sentimentSigma = this->sentimentStationarySd * std::sqrt(2.0 * this->sentimentTheta);
}
void Agent::updateSentiment() {
	double s0 = this->sentiment;
	double mu = this->OB.marketNeutralSentiment;
	double theta = this->sentimentTheta;
	double r = this->reactionTime;
	double sigma = this->sentimentSigma;
	double decay = std::exp(-theta * r);
	double noiseScaling = std::sqrt((1 - std::exp((-2 * theta) * r)) / (2 * theta));
	double n = sampleNormal();

	double newSentiment = mu + (s0 - mu) * decay + sigma * noiseScaling * n;
	this->sentiment = newSentiment;

	// Advance the running average over the same interval the OU step just covered.
	// alpha is derived from elapsed sim time, not taken as a fixed weight, so agents on
	// wildly different schedules end up with averages over the same real window: an agent
	// acting every second and one acting every ten minutes both cover half a step change
	// in SENTIMENT_EWMA_HALFLIFE_MINUTES.
	//
	// This is the standard irregular interval EWMA, which treats the new reading as
	// representative of the interval that just ended. It therefore leads a true time
	// average slightly, which is the wanted behaviour here since the point is to measure
	// recent conviction rather than to integrate the past exactly.
	double alpha = 1.0 - std::exp(-r / SENTIMENT_EWMA_TAU_MS);
	this->sentimentEwma += alpha * (this->sentiment - this->sentimentEwma);
}
double Agent::adversity() const {
	// Positive when the averaged sentiment agrees with the way this agent is positioned
	double conviction = this->directionalBias * this->sentimentEwma;

	// Only the adverse direction counts, and only up to fully opposed
	double adverse = -conviction;
	if (adverse < 0.0) { return 0.0; }
	if (adverse > 1.0) { return 1.0; }
	return adverse;
}
OrderAction Agent::sentimentToAction() {
	double boundSentiment = std::max(-1.0, std::min(1.0, this->sentiment));

	std::vector<double> scores(sentimentActions.size());
	for (int i = 0; i < scores.size(); ++i) {
		double diff = boundSentiment - this->sentimentAnchors[i];
		scores[i] = -(diff * diff) / this->sentimentTemperature;
	}

	double maxScore = *std::max_element(scores.begin(), scores.end());

	std::vector<double> expScores(sentimentActions.size());
	double totalScore = 0.0;
	for (int i = 0; i < expScores.size(); ++i) {
		expScores[i] = std::exp(scores[i] - maxScore);
		totalScore += expScores[i];
	}

	std::vector<double> probs(this->sentimentActions.size());
	for (int i = 0; i < probs.size(); ++i) {
		probs[i] = expScores[i] / totalScore;
	}

	// Random weighted choice
	int choice = sampleDiscrete(probs);

	return this->sentimentActions[choice];
}
OrderAction Agent::getRandomAction() {
	bool hasCash = this->cash >= this->OB.currentPrice;
	bool hasHolding = this->getTotalHoldings() > 0;
	bool hasActiveOrder = !this->activeAsks.empty() || !this->activeBids.empty();
	OrderAction action = OrderAction::HOLD;

	// Check if agent is bankrupt
	if (!hasCash && !hasHolding && !hasActiveOrder) {
		if (this->status != AgentStatus::BANKRUPT) { this->status = AgentStatus::BANKRUPT; }
		return action;
	}

	this->updateSentiment();

	action = this->sentimentToAction();

	// Sanitize action choice
	if (action == OrderAction::BID && !hasCash) { action = OrderAction::HOLD; }
	if (action == OrderAction::ASK && !hasHolding) { action = OrderAction::HOLD; }

	// Check if agent should cancel an active order
	if (action == OrderAction::HOLD && hasActiveOrder && randomInt(0, 3) == 0) {
		action = OrderAction::CANCEL;
	}

	return action;
}
OrderAction Agent::getRandomAction_DEPRECATED() {
	std::vector<OrderAction> availableActions = { OrderAction::HOLD };
	int totalHoldings = this->getTotalHoldings();
	int actionChoice = 0;
	int numActions = 1;

	if (this->cash >= this->OB.currentPrice) { availableActions.push_back(OrderAction::BID); }
	if (totalHoldings > 0) { availableActions.push_back(OrderAction::ASK); }
	if (!this->activeAsks.empty() || !this->activeBids.empty()) { availableActions.push_back(OrderAction::CANCEL); }

	numActions = availableActions.size();
	if (numActions == 1) { actionChoice = 0; }
	else { actionChoice = randomInt(0, numActions - 1); }

	return availableActions[actionChoice];
}
std::shared_ptr<Order> Agent::makeMarketBid() {
	int maxPurchasable = int(this->cash / this->OB.currentPrice);
	if (maxPurchasable < 1) { return nullptr; }

	int chosenVol = randomInt(1, maxPurchasable);

	std::shared_ptr<Order> order = std::make_shared<Order>(
		this->OB.makeId(ID_TYPE::ORDER),
		this->id,
		-1,
		chosenVol,
		this->OB.clock->simTimeMs,
		OrderAction::BID,
		OrderType::MARKET
	);

	return order;
}
std::shared_ptr<Order> Agent::makeLimitBid(bool forceAggressive, bool fullSize) {
	// Crossing orders are priced off the opposite touch, passive ones off the last trade
	double chosenPrice = (forceAggressive || this->rollAggressive())
		? this->getMarketablePrice(OrderAction::BID) : -1.0;
	if (chosenPrice <= 0.0) { chosenPrice = this->getBetaPrice(this->OB.currentPrice, OrderAction::BID); }

	int maxPurchasable = int(this->cash / chosenPrice);

	// Crossing costs more than resting. An agent that cannot afford to take
	// liquidity posts passively instead of sitting the turn out.
	if (maxPurchasable < 1) {
		chosenPrice = this->getBetaPrice(this->OB.currentPrice, OrderAction::BID);
		maxPurchasable = int(this->cash / chosenPrice);
	}
	if (maxPurchasable < 1) { return nullptr; }

	int chosenVol = fullSize ? maxPurchasable : randomInt(1, maxPurchasable);
	double totalValue = roundTo(chosenPrice * chosenVol);

	std::shared_ptr<Order> order = std::make_shared<Order>(
		this->OB.makeId(ID_TYPE::ORDER),
		this->id,
		chosenPrice,
		chosenVol,
		this->OB.clock->simTimeMs,
		OrderAction::BID,
		OrderType::LIMIT,
		std::vector<Holding>{},
		this->rollOrderExpiry(this->OB.clock->simTimeMs)
	);

	this->updateCash(-totalValue);

	return order;
}
std::shared_ptr<Order> Agent::makeMarketAsk() {
	int chosenVol = 1;
	int totalHoldings = this->getTotalHoldings();
	if (totalHoldings < 1) { return nullptr; }
	if (totalHoldings > 1) { chosenVol = randomInt(1, totalHoldings); }

	std::vector<Holding> reservedHoldings = this->removeHoldings(chosenVol);

	std::shared_ptr<Order> order = std::make_shared<Order>(
		this->OB.makeId(ID_TYPE::ORDER),
		this->id,
		-1,
		chosenVol,
		this->OB.clock->simTimeMs,
		OrderAction::ASK,
		OrderType::MARKET,
		reservedHoldings
	);

	return order;
}
std::shared_ptr<Order> Agent::makeLimitAsk(bool forceAggressive, bool fullSize) {
	// Crossing orders are priced off the opposite touch, passive ones off the last trade
	double chosenPrice = (forceAggressive || this->rollAggressive())
		? this->getMarketablePrice(OrderAction::ASK) : -1.0;
	if (chosenPrice <= 0.0) { chosenPrice = this->getBetaPrice(this->OB.currentPrice, OrderAction::ASK); }

	int chosenVol = 1;

	int totalHoldings = this->getTotalHoldings();
	if (totalHoldings < 1) { return nullptr; }
	if (fullSize) { chosenVol = totalHoldings; }
	else if (totalHoldings > 1) { chosenVol = randomInt(1, totalHoldings); }

	std::vector<Holding> reservedHoldings = this->removeHoldings(chosenVol);

	std::shared_ptr<Order> order = std::make_shared<Order>(
		this->OB.makeId(ID_TYPE::ORDER),
		this->id,
		chosenPrice,
		chosenVol,
		this->OB.clock->simTimeMs,
		OrderAction::ASK,
		OrderType::LIMIT,
		reservedHoldings,
		this->rollOrderExpiry(this->OB.clock->simTimeMs)
	);

	return order;
}
void Agent::cancelOrder() {
	bool hasBids = !activeBids.empty();
	bool hasAsks = !activeAsks.empty();

	if (!hasBids && !hasAsks) { return; }
	
	bool side = (hasBids && hasAsks) ? randomInt(0, 1) : (hasBids ? 0 : 1);

	if (side) {
		auto it = activeAsks.begin();
		std::advance(it, randomInt(0, static_cast<int>(activeAsks.size() - 1)));
		this->OB.cancelOrder(it->second, shared_from_this());
	}
	else {
		auto it = activeBids.begin();
		std::advance(it, randomInt(0, static_cast<int>(activeBids.size() - 1)));
		this->OB.cancelOrder(it->second, shared_from_this());
	}
}
void Agent::hold() {
	return;
}

// ---- Utility Operations ----

double Agent::rollOrderExpiry(double nowMs) {
	// Day order equivalent, expires when the current session's expiring boundary is reached
	double expiry = MarketCalendar::nextExpiryBoundaryMs(nowMs);

	// Each extra boundary survived is a coin flip, so at most half of all orders
	// survive any single boundary, half of those survive the next, and so on
	for (int i = 0; i < MAX_EXPIRY_BOUNDARIES_SURVIVED; ++i) {
		if (randomInt(0, 1) != 0) { break; }
		expiry = MarketCalendar::nextExpiryBoundaryMs(expiry);
	}

	return expiry;
}
double Agent::getBetaPrice(double currentPrice, OrderAction side, double a, double b, double epsilon) {
	double x, discount, premium, preRounded, precision, betaPrice;
	double maxVariance = this->getMaxVariance(currentPrice);

	switch (side) {
	case OrderAction::BID:
		x = sampleBeta(a, b);
		discount = x * maxVariance;
		preRounded = std::max(currentPrice * (1 - discount), epsilon);
		precision = (preRounded < 1.00) ? 0.0001 : 0.01;
		betaPrice = roundTo(preRounded, precision);
		return betaPrice;
	case OrderAction::ASK:
		x = sampleBeta(a, b);
		premium = x * maxVariance;
		preRounded = currentPrice * (1 + premium);
		precision = (preRounded < 1.00) ? 0.0001 : 0.01;
		betaPrice = roundTo(preRounded, precision);
		return betaPrice;
	}
	return roundTo(currentPrice, this->OB.tickPrecision);
}
bool Agent::rollAggressive() {
	double aggression = 0.0;

	switch (this->subType) {
	case AgentSubType::NOISE:    aggression = AGGRESSION_NOISE;    break;
	case AgentSubType::MOMENTUM: aggression = AGGRESSION_MOMENTUM; break;
	case AgentSubType::INFORMED: aggression = AGGRESSION_INFORMED; break;
	case AgentSubType::ALGO:     aggression = AGGRESSION_ALGO;     break;
	}

	return randomDouble(0.0, 1.0) < aggression;
}
double Agent::participationRate(Session session, double simTimeMs) const {
	// The whole population trades the main session
	if (session == Session::REGULAR) { return 1.0; }

	double base = 0.0;
	if (this->type == AgentType::INSTITUTION) {
		base = (this->subType == AgentSubType::ALGO)
			? EXT_PARTICIPATION_INST_ALGO
			: EXT_PARTICIPATION_INST_INFORMED;

		// Institutional desks cover extended hours consistently, no taper
		return base;
	}

	switch (this->subType) {
	case AgentSubType::ALGO:     base = EXT_PARTICIPATION_RETAIL_ALGO;     break;
	case AgentSubType::INFORMED: base = EXT_PARTICIPATION_RETAIL_INFORMED; break;
	case AgentSubType::MOMENTUM: base = EXT_PARTICIPATION_RETAIL_MOMENTUM; break;
	default:                     base = EXT_PARTICIPATION_RETAIL_NOISE;    break;
	}

	// Retail drains away after the close and refills into the open. Progress runs
	// 0 (full base rate) to 1 (floor), so premarket is the mirror of the decay.
	double progress = (session == Session::PREMARKET)
		? 1.0 - MarketCalendar::premarketProgress(simTimeMs)
		: MarketCalendar::closingDecayProgress(simTimeMs);

	return base * std::pow(RETAIL_EXTENDED_FLOOR_FACTOR, progress);
}
bool Agent::isParticipating(Session session, double simTimeMs) const {
	return this->participationThreshold < this->participationRate(session, simTimeMs);
}
double Agent::getMarketablePrice(OrderAction side, double epsilon) {
	// Price against the opposite touch, that is what makes the order marketable
	OrderAction oppositeSide = (side == OrderAction::BID) ? OrderAction::ASK : OrderAction::BID;
	std::vector<std::shared_ptr<Order>> opposite = this->OB.peekBestN(oppositeSide, 1);

	// Nothing to cross, caller falls back to a passive price
	if (opposite.empty() || opposite[0] == nullptr) { return -1.0; }

	double touch = opposite[0]->price;
	if (touch <= 0.0) { return -1.0; }

	// Willingness to reach past the touch, most orders take only the top level
	double slip = sampleBeta(2.0, 5.0) * this->getMaxVariance(touch) * MARKETABLE_SLIP_SCALE;

	double preRounded = (side == OrderAction::BID)
		? touch * (1.0 + slip)
		: std::max(touch * (1.0 - slip), epsilon);

	double precision = (preRounded < 1.00) ? 0.0001 : 0.01;
	return roundTo(preRounded, precision);
}
double Agent::getMaxVariance(double price, double scale, double decayRate, double amplitude, double frequency) {
	return scale * (pow(price, -decayRate)) * (1 + (amplitude * sin(frequency * log(price))));
}