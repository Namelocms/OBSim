#pragma once
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include "Enums.h" // TimeInForce and SessionMask, needed complete for the defaults below

class Holding;
class EnumStrings;
enum class OrderStatus;
enum class OrderAction;
enum class OrderType;

class Order {
public:
	const std::string id;
	const std::string agentId;
	double price;
	unsigned int volume;
	const unsigned int entryVolume;
	double timestamp;
	OrderStatus status;
	const OrderAction side;
	const OrderType type;
	std::vector<Holding> reservedShares;
	/* Sim time this order is cancelled at, set from the expiring session boundary it was rolled for.
	*  Only meaningful for limit orders, market orders never rest in the book. */
	double expiresAtMs;
	/* How long this order lives and whether it may rest. See TimeInForce. */
	TimeInForce tif;
	/* Sessions this order may trade in. Outside them it rests but is not matched. */
	SessionMask sessions;
	/* For an ask: long sale, located short, or exempt market maker short. Always LONG for a bid. */
	SaleMark mark = SaleMark::LONG;
	bool isShortSale() const { return this->side == OrderAction::ASK && this->mark != SaleMark::LONG; }

	// ---- Fees (OrderModelPlan Step 1.1), all zero with fees off ----
	//
	// A commission minimum, a TAF cap and rounding are defined per ORDER, not per fill, so
	// the order carries what it has filled and what it has been charged across its legs.

	/* Shares this order has filled so far, and for how much in total */
	unsigned int filledVolume = 0;
	double filledValue = 0.0;
	/* Commission and TAF accrued so far, unrounded, so each leg charges only the increase */
	double commissionAccrued = 0.0;
	double tafAccrued = 0.0;
	/* All fees accrued so far, unrounded (rebates negative), and what has actually been charged
	*  in whole cents. Each leg charges round(accrued) - charged. */
	double feeAccrued = 0.0;
	double feeCharged = 0.0;
	/* Cash a limit bid escrowed for its worst-case fees on top of its price, drawn down as
	*  fees are charged and refunded with whatever is left when the order closes or is cancelled */
	double feeReserve = 0.0;

	Order() = default;
	Order(
		const std::string id,
		const std::string agentId,
		double price,
		unsigned int volume,
		double timestamp,
		const OrderAction side,
		const OrderType type,
		std::vector<Holding> reservedShares = {},
		double expiresAtMs = 0.0,
		TimeInForce tif = TimeInForce::GTD,
		SessionMask sessions = SESSIONS_ALL
	);
	/* May this order trade in the given session? */
	bool eligibleIn(Session session) const { return (this->sessions & sessionBit(session)) != 0; }

	/* Get unsold shares from the calling Order */
	std::vector<Holding> getReturnableShares();
	/* Shrink the reserved lots to exactly `keep` shares, returning the lots released
	*
	* Rebuilds reservedShares from the order's live lots (getReturnableShares), so afterwards
	* the reserved lots total exactly the order's live size, which is the state a resize
	* starts from. Used by a replace that changes an ask's size.
	*/
	std::vector<Holding> trimReserved(unsigned int keep);
	/* Get information about the order as a string */
	std::string toString() const;
};

