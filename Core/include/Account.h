#pragma once

class Agent;

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
namespace Account {

/* Cash this account has pre-debited into its open limit bids. Still the agent's money. */
double escrowedCash(const Agent& agent);

/* Shares this account owns: those it holds plus those reserved in its open asks */
unsigned long long longShares(const Agent& agent);

/* Signed net position in shares: long minus short. Long only until OrderModelPlan Step 1.4. */
long long netShares(const Agent& agent);

/* Could this account open a short position right now?
*
* Always false until OrderModelPlan Step 1.4 builds short selling, where it becomes: margin
* privileges, a locate, and buying power for the requirement. Read today only by the
* transient arrival rule, whose bearish branch is therefore unreachable until then.
*/
bool canSellShort(const Agent& agent);

/* What the account is worth at the given price
*
*     equity = cash + escrowedCash + longShares * price
*/
double equity(const Agent& agent, double price);

/* Cash this account may commit to new purchases. A cash account: its cash, never negative. */
double buyingPower(const Agent& agent);

/* Most shares this account could buy at the given price, 0 for a non-positive price
*
* Clamped to INT_MAX, since agents draw a size with randomInt. The old sizing was
* int(cash / price), which is undefined above INT_MAX shares (a large account at a
* sub-cent price) and on MSVC came out as INT_MIN, silently sitting the agent out.
*/
unsigned int affordableVolume(const Agent& agent, double price);

}
