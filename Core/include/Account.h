#pragma once

class Agent;
class Order;
struct FeeSchedule;

/* ---- Account ----
*
* What an agent's balances MEAN, defined once.
*
* The balances themselves stay on Agent (cash, holdings, the active order maps), because the
* frozen terminal UI and the Bridge read them there. Everything that interprets them --
* how much the account is worth, how much it may still spend, how many shares it really
* owns -- is defined here and nowhere else. Margin, short positions and their requirements
* (OrderModelPlan Phase 1) extend these functions rather than adding a second definition
* beside them.
*
* Today every account is a cash account, so cash is never negative and buying power is
* simply cash. Code that sizes an order must still ask buyingPower() rather than reading
* cash: once a margin debit can make cash negative, `cash / price` goes negative or wraps,
* and that is a bug this indirection exists to never let happen.
*/
/* ---- Margin rules (OrderModelPlan Step 1.2) ----
*
* The regulator's rules, as published, not tuned:
*
*   Reg T            50% initial requirement on a margin purchase
*   FINRA 4210(b)    $2,000 minimum equity to borrow at all
*   FINRA 4210(c)    25% maintenance on longs, as a floor under the broker's own figure
*
* and two broker policies that are near universal, so they are the defaults:
*
*   stock under $5 is not marginable: it cannot be bought on margin, and once a stock falls
*   below $5 a position already carried on margin is held at 100% -- the whole loan comes
*   due. That is decision D4 in the plan: the default $1.00 market is a penny stock, and is
*   treated as one.
*
*   a margin call is met by liquidating enough to restore the INITIAL requirement, not merely
*   maintenance, which would re-trigger on the next tick (decision D3).
*
* MARGIN_MIN_EQUITY is in UNIT dollars -- equity divided by the run's cash scale, the person
* an agent stands for -- so the share of accounts that qualify does not move with
* agentStartCount. An absolute dollar figure in a model whose scale is configurable is the
* bug DispersalPlan fixed twice.
*/
inline constexpr double REG_T_INITIAL = 0.50;
inline constexpr double FINRA_MAINTENANCE_LONG = 0.25;
inline constexpr double MARGIN_MIN_EQUITY = 2'000.0;
inline constexpr double MARGINABLE_MIN_PRICE = 5.00;
/* FINRA 4210(c)(2): maintenance on a short, per share. Below $5: the greater of $2.50 or the
*  price -- 250% of a $1 position. At or above $5: the greater of $5.00 or 30%. */
inline constexpr double SHORT_LOW_PRICE_FLOOR = 2.50;
inline constexpr double SHORT_HIGH_PRICE_FLOOR = 5.00;
inline constexpr double SHORT_MAINTENANCE_PCT = 0.30;
/* Share of the initial requirement a margin call liquidates back up to (D3) */
inline constexpr double LIQUIDATION_TARGET = REG_T_INITIAL;
/* How far through the best bid a liquidation outside the regular session will reach, where
*  market orders are not accepted. A safety bound, not a market parameter. */
inline constexpr double LIQUIDATION_OUTSIDE_SLIP = 0.05;

namespace Account {

/* Cash this account has pre-debited into its open limit bids. Still the agent's money. */
double escrowedCash(const Agent& agent);

/* Shares this account owns: those it holds plus those reserved in its open asks */
unsigned long long longShares(const Agent& agent);

/* Signed net position in shares: long minus short */
long long netShares(const Agent& agent);

/* Are short selling and lending switched on? They need margin on as well. */
bool shortingEnabled(const Agent& agent);

/* Could this account open a short position right now?
*
* Always false until OrderModelPlan Step 1.4 builds short selling, where it becomes: margin
* privileges, a locate, and buying power for the requirement. Read today only by the
* transient arrival rule, whose bearish branch is therefore unreachable until then.
*/
bool canSellShort(const Agent& agent);

/* What the account is worth at the given price
*
*     equity = cash + escrowedCash + (longShares - shortShares) * price
*/
double equity(const Agent& agent, double price);

/* Cash this account may commit to new purchases
*
* A cash account: its cash, never negative. A margin account in marginable stock: its excess
* equity over the Reg T initial requirement on everything it holds and has bid for, divided
* by that requirement -- twice its excess, at 50%.
*/
double buyingPower(const Agent& agent);

/* Margin on, and this account has the FINRA minimum equity (in unit dollars) to borrow? */
bool hasMarginPrivileges(const Agent& agent);

/* Can this stock be bought on margin at this price? */
bool isMarginable(double price);

/* Maintenance requirement fraction on longs at this price: the larger of FINRA's floor and
*  the broker's house figure, or 100% for stock that is not marginable */
double longMaintenance(const Agent& agent, double price);

/* Dollars the account must keep as equity at this price */
double maintenanceRequirement(const Agent& agent, double price);

/* Is the account below its maintenance requirement at this price? */
bool inMaintenanceViolation(const Agent& agent, double price);

/* The price below which a long margin account falls into violation, or 0 if there is none
*
* With C = cash + escrowedCash (negative: a loan) and L long shares, equity C + L*P falls
* below m*L*P when P < -C / (L * (1 - m)). Under MARGINABLE_MIN_PRICE the requirement is
* 100% and any loan is a violation, so the trigger is never below that line.
*/
double liquidationPrice(const Agent& agent);

/* Margin loan outstanding: what the account has borrowed beyond its own cash and escrow */
double debitBalance(const Agent& agent);

// ---- Short selling (OrderModelPlan Step 1.4) ----

/* FINRA maintenance on a short, per share, at this price */
double shortMaintenancePerShare(double price);
/* Equity a NEW short needs per share: Reg T's 50% beyond the proceeds, or maintenance if that
*  is more -- which it is for any stock under $10 */
double shortOpeningPerShare(double price);
/* Shares this account has offered short and not yet sold */
unsigned long long pendingShortShares(const Agent& agent);
/* Shares this account could sell short right now on its margin, before any locate */
unsigned int shortCapacity(const Agent& agent);
/* Is this account a market maker, whose shorts are exempt from the locate? Institutional ALGO
*  only: a retail algorithmic trader is not a registered market maker. */
bool isExemptMarketMaker(const Agent& agent);
/* Shares of this account's short not yet bid for: what a new covering bid may be for */
unsigned int coverableShares(const Agent& agent);
/* The price ABOVE which a short account falls into violation, or 0 if there is none */
double shortLiquidationPrice(const Agent& agent);
/* Shares a margin call on a short has to buy back, at this price, to restore the opening
*  requirement */
unsigned int shortCoverShares(const Agent& agent, double price);

/* Shares a margin call has to sell, at the current price, to restore LIQUIDATION_TARGET */
unsigned int liquidationShares(const Agent& agent, double price);

/* The fee schedule this account pays: its type's schedule from Features::fees */
const FeeSchedule& feeSchedule(const Agent& agent);

/* Are fees switched on for this account's market? */
bool feesEnabled(const Agent& agent);

/* Refund what is left of a bid's fee reserve to its owner and zero it */
void releaseFeeReserve(Agent& agent, Order& order);

/* Most shares this account could buy at the given price, 0 for a non-positive price
*
* Clamped to INT_MAX, since agents draw a size with randomInt. The old sizing was
* int(cash / price), which is undefined above INT_MAX shares (a large account at a
* sub-cent price) and on MSVC came out as INT_MIN, silently sitting the agent out.
*
* With fees on, sized so the order can also carry its worst-case fees, using the deliberately
* over-estimated FeeSchedule::buyFeePerShare and buyFeeFixed, plus two cents of rounding slack.
*/
unsigned int affordableVolume(const Agent& agent, double price);

}
