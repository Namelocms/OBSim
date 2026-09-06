#pragma once
#include <unordered_map>
#include <string>
#include <memory>
#include <vector>

enum class OrderAction;
enum class AgentStatus;
enum class AgentType;
enum class AgentSubType;
enum class Session;
class OrderBook;
class MatchingEngine;
class Order;
class Holding;

const double PI = 3.14159265358979323846;

/* ---- Marketable limit aggression ----
*
* Probability an agent prices a limit order to CROSS the spread rather than rest
* passively behind it. Flow that wants immediate execution sends a marketable
* limit, priced through the opposite touch, rather than a naked market order.
* That is how institutions and algos take liquidity while still capping slippage,
* and it is the only crossing mechanism available outside regular hours.
*
* Passive quoting agents (ALGO) should almost never cross, takers cross readily,
* and informed traders cross most since their edge decays.
*/
inline constexpr double AGGRESSION_NOISE = 0.20;
inline constexpr double AGGRESSION_MOMENTUM = 0.30;
inline constexpr double AGGRESSION_INFORMED = 0.40;
inline constexpr double AGGRESSION_ALGO = 0.02;
/* How far past the opposite touch a marketable order will reach, as a fraction
*  of the agent's normal price variance. This is the slippage cap. */
inline constexpr double MARKETABLE_SLIP_SCALE = 0.50;

/* ---- Extended hours participation ----
*
* Outside the regular session the market does not merely slow down, it has far
* fewer people in it. Each agent holds a fixed participation threshold, and takes
* part only while its kind's participation rate for the current session sits above
* that threshold. The threshold is drawn once at creation, so the ramp is smooth
* and reproducible and the same agents are the reliable extended hours traders
* every day rather than a fresh random draw each session.
*
* Institutional desks cover extended hours consistently, so their rate is flat.
* Retail tapers away after the close, bottoms out overnight, and builds back
* through premarket, which is what makes premarket institution and news driven.
*/
inline constexpr double EXT_PARTICIPATION_INST_ALGO = 0.40;
inline constexpr double EXT_PARTICIPATION_INST_INFORMED = 0.35;
inline constexpr double EXT_PARTICIPATION_RETAIL_ALGO = 0.20;
inline constexpr double EXT_PARTICIPATION_RETAIL_INFORMED = 0.15;
inline constexpr double EXT_PARTICIPATION_RETAIL_MOMENTUM = 0.05;
inline constexpr double EXT_PARTICIPATION_RETAIL_NOISE = 0.03;
/* Share of its base rate retail retains at the deepest point of the overnight */
inline constexpr double RETAIL_EXTENDED_FLOOR_FACTOR = 0.20;

/* ---- Sentiment averaging ----
*
* Agents act on irregular schedules, from sub-millisecond to over an hour apart, so a
* plain per-action average would weight a fast agent's recent mood far more heavily
* than a slow one's. The EWMA is therefore driven by ELAPSED SIM TIME rather than by
* action count: every agent's average covers the same real window regardless of how
* often it happens to act.
*
* Half-life is how long it takes the average to cover half the distance to a new
* level. tau is the matching time constant, tau = halfLife / ln 2.
*/
inline constexpr double SENTIMENT_EWMA_HALFLIFE_MINUTES = 20.0;
inline constexpr double SENTIMENT_EWMA_TAU_MS =
	(SENTIMENT_EWMA_HALFLIFE_MINUTES * 60'000.0) / 0.693147180559945309417;

/* ---- Transient agents ----
*
* The resident population is fixed for the life of a run, which makes activity a constant
* function of agentStartCount. Transient agents are the floating part: they arrive, trade
* for a drawn tenure, and leave again.
*
* They are RETAIL only, and never ALGO. Market making infrastructure does not come and go
* for an hour at a time, so the transient mix is takers: noise traders and momentum
* chasers, with a small informed minority.
*/
inline constexpr double TRANSIENT_MIX_NOISE = 0.50;
inline constexpr double TRANSIENT_MIX_MOMENTUM = 0.40;
/* INFORMED takes the remainder, so the three always sum to 1 */

/* Reaction floors, deliberately compressed against resident retail's 200 ms - 1 hr.
*
* A trader who has just deliberately shown up to trade a move acts on a minutes cadence,
* not an hourly one. This is not cosmetic: with a mean tenure near 48 minutes, resident
* retail reaction times would give most transient agents a single action before they left,
* which is exactly the degenerate "arrive, trade once, leave" behaviour being avoided.
*/
inline constexpr double TRANSIENT_REACTION_MIN_NOISE_MS = 2'000.0;
inline constexpr double TRANSIENT_REACTION_MAX_NOISE_MS = 600'000.0;
inline constexpr double TRANSIENT_REACTION_MIN_MOMENTUM_MS = 2'000.0;
inline constexpr double TRANSIENT_REACTION_MAX_MOMENTUM_MS = 300'000.0;
inline constexpr double TRANSIENT_REACTION_MIN_INFORMED_MS = 1'000.0;
inline constexpr double TRANSIENT_REACTION_MAX_INFORMED_MS = 300'000.0;

/* Day trader sized, a little above the resident retail median */
inline constexpr double TRANSIENT_CASH_MIN = 200.0;
inline constexpr double TRANSIENT_CASH_MAX = 5'000.0;

/* ---- Tenure ----
*
* Tenure is a fixed commitment window followed by an exponential hazard, not a deadline
* drawn up front, so that adversity can shorten it while the agent is still in the market.
*
* The floor is what stops the distribution being degenerate. A bare exponential has its
* mode at zero, so without it most transient agents would leave almost immediately.
*/
inline constexpr double TRANSIENT_MIN_TENURE_MINUTES = 5.0;
inline constexpr double TRANSIENT_TENURE_HALFLIFE_MINUTES = 30.0;
/* Fractional spread of the per-agent half-life around the value above, so the population
*  holds both scalpers and agents that sit for hours rather than one uniform lifespan */
inline constexpr double TRANSIENT_TENURE_HALFLIFE_SPREAD = 0.50;

/* Lowest conviction a transient agent can arrive with, as |sentiment|
*
* They turned up because they wanted to trade, so they do not arrive neutral. Signed by
* directionalBias, so a future short biased agent arrives bearish by the same rule.
*/
inline constexpr double TRANSIENT_ARRIVAL_SENTIMENT_MIN = 0.10;
inline constexpr double TRANSIENT_ARRIVAL_SENTIMENT_MAX = 0.90;

/* Mean undisturbed tenure, the commitment window plus the exponential's mean
*
* Per-agent half-lives are drawn symmetrically about TRANSIENT_TENURE_HALFLIFE_MINUTES, so
* the population mean is that value, and Exponential(halfLife) has mean halfLife / ln 2.
*
* "Undisturbed" matters: adversity shortens real tenures, so the realised concurrent
* population sits BELOW the target this feeds. The arrival rate is not corrected for that,
* because the shortfall is the feature working.
*/
inline constexpr double TRANSIENT_MEAN_TENURE_MINUTES = TRANSIENT_MIN_TENURE_MINUTES
	+ (TRANSIENT_TENURE_HALFLIFE_MINUTES / 0.693147180559945309417);

/* ---- Arrivals ----
*
* Arrival rate is expressed as a target STEADY-STATE CONCURRENCY, as a share of the
* resident population, rather than as an absolute rate. Little's law then gives the rate:
*
*     arrivalsPerHour = (fraction * residents / meanTenureHours) * sessionFactor
*
* Stating it this way means the knob is the number a person actually reasons about ("how
* much of the crowd is transient"), and transient activity stays proportional as
* agentStartCount changes instead of needing retuning at every population size.
*/
inline constexpr double TRANSIENT_DEFAULT_FRACTION = 0.08;
/* Hard ceiling on LIVE transient agents, as a share of residents. Pooled slots do not count.
*
* Defense in depth like the back-data caps, not a working limit: steady state sits near
* TRANSIENT_DEFAULT_FRACTION, well under this. The headroom multiple keeps the ceiling
* clear of the target if the fraction is ever raised, so it cannot silently start binding.
*/
inline constexpr double TRANSIENT_MAX_POPULATION_FRACTION = 0.25;
inline constexpr double TRANSIENT_CAP_HEADROOM = 3.0;

/* Per-session multipliers on the arrival rate.
*
* Explicit rather than reusing the retail participation curve. That curve bottoms out near
* 0.006, which would put premarket arrivals at effectively zero -- but premarket is exactly
* when news driven day traders turn up. Fewer people are around off hours, not none.
*/
inline constexpr double TRANSIENT_SESSION_FACTOR_PREMARKET = 0.10;
inline constexpr double TRANSIENT_SESSION_FACTOR_REGULAR = 1.00;
inline constexpr double TRANSIENT_SESSION_FACTOR_AFTERHOURS = 0.08;
inline constexpr double TRANSIENT_SESSION_FACTOR_OVERNIGHT = 0.01;

class Agent : public std::enable_shared_from_this<Agent> {
public:
	/* Agent's unique ID */
	const std::string id;
	/* Agent's minimum reaction time in milliseconds */
	double reactionTimeFloor;
	/* Agent's currently used reaction time with jitter added */
	double reactionTime;
	/* Reaction floor this agent backs off to after its opening action, 0 disables the backoff
	*
	* Passive quoting agents (ALGO) only need their fast floor to claim an early slot
	* in the opening queue. Once they have quotes resting there is nothing to do until
	* a fill wakes them, so leaving them on a sub-millisecond timer burns enormous
	* amounts of compute producing HOLDs.
	*/
	double idleReactionTimeFloor = 0.0;
	/* Number of actions this agent has taken, drives the opening action backoff */
	unsigned long long actionCount = 0;
	/* Generation of this agent's live event, anything older in the queue is stale
	*
	* std::priority_queue cannot remove a scheduled event, so waking an agent early
	* pushes a newer event and bumps this counter to invalidate the old one.
	*
	* BUMP ON RECYCLE, NEVER RESET. An agent id that is reused for a new personality
	* can still have events from the previous incarnation sitting in the queue. Bumping
	* leaves them stale so they are discarded on pop; resetting to zero would let one of
	* them alias a live event and fire against the wrong agent.
	*/
	unsigned long long eventGeneration = 0;
	/* True while this agent has a live event sitting in the queue */
	bool hasPendingEvent = false;
	/* Fixed draw in [0,1) deciding how committed this agent is to trading off hours
	*
	* Compared against its kind's participation rate for the current session, so
	* the population thins and refills smoothly without re-rolling anyone.
	*/
	double participationThreshold;
	/* Agent's buying power */
	double cash;
	/* Agent's market sentiment */
	double sentiment;
	/* Time-weighted average of this agent's sentiment, half-life SENTIMENT_EWMA_HALFLIFE_MINUTES
	*
	* Seeded to the agent's opening sentiment rather than to zero, so a freshly created
	* agent reads as holding the conviction it was born with instead of a neutral prior
	* it never had.
	*/
	double sentimentEwma = 0.0;
	/* Which way this agent's book leans: +1 long biased, -1 short biased
	*
	* Sentiment on its own cannot say whether the market is going an agent's way, because
	* that depends on which way the agent is positioned. Falling sentiment is adverse to a
	* long and favourable to a short. Every stance relative calculation therefore goes
	* through this rather than testing the sign of sentiment directly, so adding a short
	* side agent later is a bias flip and not a rewrite.
	*
	* Nothing sets this to -1 yet. It is deliberately a double rather than a bool or an
	* enum so a partially hedged agent can later sit somewhere between the two.
	*/
	double directionalBias = 1.0;
	/* Agent's speed of sentiment mean reversion to OB.marketNeutralSentiment, higher = faster */
	double sentimentTheta;
	/* Agent's sentiment volatility, higher = larger swings */
	double sentimentSigma;
	/* Controls agent's randomness and sharpness of the sentiment probability distribution */
	double sentimentTemperature;
	/* Agent's status */
	AgentStatus status;
	/* True while this agent slot is a transient participant rather than a resident
	*
	* Stays true across pooling and rerolling: the slot remains a transient slot, it is
	* just unoccupied between incarnations.
	*/
	bool isTransient = false;
	/* Which incarnation of this slot is currently live, bumped by every reroll
	*
	* An agent id is reused, so this is what distinguishes one occupant of a slot from the
	* next. Anything attributed to a slot should carry the incarnation it was created under.
	*/
	unsigned long long incarnation = 0;
	/* Sim time this incarnation entered the market */
	double arrivedAtMs = 0.0;
	/* Sim time the commitment window closes and the departure hazard starts running */
	double minTenureEndsMs = 0.0;
	/* This agent's own tenure half-life in ms, drawn per incarnation */
	double tenureHalfLifeMs = 0.0;
	/* Sim time the departure hazard was last evaluated, the hazard integrates from here */
	double lastDepartCheckMs = 0.0;
	/* Sim time this agent started unwinding, 0 while it is not leaving */
	double leavingSinceMs = 0.0;
	/* Agent main type */
	AgentType type;
	/* Agent sub-type */
	AgentSubType subType;
	/* All shares the agent currently holds */
	std::unordered_map<double, Holding> holdings;
	/* All open ask orders placed by the agent */
	std::unordered_map<std::string, std::shared_ptr<Order>> activeAsks;
	/* All open bid orders placed by the agent */
	std::unordered_map<std::string, std::shared_ptr<Order>> activeBids;
	/* The orderbook for the current stock */
	OrderBook& OB;
	/* The matching engine */
	MatchingEngine& ME;

	Agent() = default;
	Agent(
		std::string id,
		double reactionTimeFloor,
		double cash,
		AgentStatus status,
		AgentType type,
		AgentSubType subType,
		OrderBook& ob,
		MatchingEngine& me
	);

// ---- Cash Operations ----
	/* Update the cash holdings of this agent [Negative amt decreses cash] */
	void updateCash(double amt);

// ---- Holdings Operations ----
	/* Update/insert a share in the agent's holdings */
	void upsertHolding(Holding holding);
	/* Remove the given volume of holdings, return the list of holdings objects of the removed shares */
	std::vector<Holding> removeHoldings(int volume);
	/* Get the number of shares the Agent is currently holding */
	unsigned int getTotalHoldings();

// ---- Active Order Operations ----
	/* Update/insert an active order */
	void upsertActiveOrder(std::shared_ptr<Order> order);
	/* Remove an active order */
	void removeActiveOrder(std::shared_ptr<Order> order);

// ---- Action Operations ----
	/* Execute a chosen action */
	void actRandom();
	/* Update the agent's sentiment value using the Ornstein-Uhlenbeck (OU) Process
	*
	* Also advances sentimentEwma over the same elapsed interval.
	*/
	void updateSentiment();
	/* How far the market has moved AGAINST this agent's stance lately, in [0, 1]
	*
	* Zero while the averaged sentiment is aligned with directionalBias or neutral, rising
	* to one when it is fully opposed. Deliberately one sided: this measures adversity, not
	* conviction, so a favourable market returns zero rather than a negative number.
	*/
	double adversity() const;
	/* Convert the raw sentiment value into a usable OrderAction enum using the SoftMax function */
	OrderAction sentimentToAction();
	/* Choose a random OrderAction given the agent's current portfolio and sentiment */
	OrderAction getRandomAction();
	/* !!DEPRECATED!! Choose a random OrderAction given the agent's current holdings and cash */
	OrderAction getRandomAction_DEPRECATED();
	/* Make a random market bid order */
	std::shared_ptr<Order> makeMarketBid();
	/* Make a random limit bid order */
	std::shared_ptr<Order> makeLimitBid();
	/* Make a random market ask order */
	std::shared_ptr<Order> makeMarketAsk();
	/* Make a random limit ask order */
	std::shared_ptr<Order> makeLimitAsk();
	/* Cancel a random order */
	void cancelOrder();
	/* Do nothing */
	void hold();

// ---- Utility Operations ----
	/* Roll the sim time a limit order placed at nowMs should expire at
	*
	* Defaults to the next expiring session boundary (the day order equivalent).
	* Each additional boundary survived is another coin flip, so no more than
	* half of all orders survive any single expiring boundary.
	*/
	double rollOrderExpiry(double nowMs);
	/* Get random price within custom beta distribution
	*
	* Can be shaped to hug the current price without reaching it: [a > b: hugs lower end || a < b: hugs upper end].
	* Has a nice distribution just a little past/before the current price is where most orders will be placed.
	* a = Higher favors right side (larger x).
	* b = Higher favors left side (smaller x).
	* epsilon = Minimum possible price.
	*/
	double getBetaPrice(double currentPrice, OrderAction side, double a = 2.0, double b = 5.0, double epsilon = 0.0001);
	/* Roll whether this order should be priced to cross the spread, weighted by subtype */
	bool rollAggressive();
	/* Get the share of this agent's kind taking part in the given session at the given time
	*
	* 1.0 during REGULAR, everyone trades the main session. Outside it, the kind's
	* base rate, decayed for retail across afterhours and overnight and grown back
	* through premarket.
	*/
	double participationRate(Session session, double simTimeMs) const;
	/* Check whether this agent is taking part right now */
	bool isParticipating(Session session, double simTimeMs) const;
	/* Get a marketable limit price, referenced to the opposite touch rather than the last trade
	*
	* Returns a price at or through the best opposite order, so the order executes
	* on arrival while still capping how far it will reach. Returns -1.0 when that
	* side of the book is empty, and the caller falls back to a passive price.
	*/
	double getMarketablePrice(OrderAction side, double epsilon = 0.0001);

private:
	/* Most additional expiring session boundaries a limit order can survive beyond its default */
	static constexpr int MAX_EXPIRY_BOUNDARIES_SURVIVED = 3;
	/* Anchor points for each agent's sentiment to action softmax calculations, [SELL, HOLD, BUY] */
	const std::vector<double> sentimentAnchors = { -1.0, 0.0, 1.0 };
	std::vector<OrderAction> sentimentActions;
	/* Get the max variance in price (how much the computed price will differ from passed price)
	*
	* scale = Scale factor, controls the overall height of the curve
		decay_rate = The power-law decay rate, larger decay_rate -> variance falls off faster as price increases
		amplitude = The sinusoidal amplitude, controls how much the variance "wiggles" above/below the base curve. Set to 0 to disable sine behavior
		frequency = The frequency of the sine (in log space), higher frequency -> more wiggles per log unit of price

		PRICE__||  max_variance
		0.1____||  0.17783
		1______||  0.10000
		10_____||  0.05623
		100____||  0.03162
		1000___||  0.01778
		10000__||  0.01000
		100000_||  0.00562
	*/
	double getMaxVariance(double price, double scale = 0.10, double decayRate = 0.25, double amplitude = 0.10, double frequency = PI * 2);

};

