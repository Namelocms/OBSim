#pragma once
#include <string>
#include "Enums.h"

/* ---- Contingent orders (OrderModelPlan Step 2.2) ----
*
* OCO, one-cancels-other: a limit leg in the book and a stop leg held by the broker, on the same
* side and for the same shares -- a take-profit and a stop-loss protecting one position. They
* cannot both reserve those shares, so the BOOK leg reserves them and the held leg shares the
* reservation (Order::reservedByGroup). A fill on the limit leg shrinks the stop leg by the
* same amount, the common broker behaviour, rather than cancelling it outright. If the stop
* triggers first, the limit leg is cancelled and the stop takes over its shares.
*
* BRACKET, one-triggers-other: an entry order whose fills create and grow an OCO pair on the
* opposite side, sized to exactly what the entry has bought (or sold short) so far, so a
* partially filled entry is protected for precisely its fill. When the pair resolves -- the
* take-profit fills or the stop-loss triggers -- whatever is left of the entry is cancelled.
*
* Members carry the group's id. A fill or a cancel of a member during matching is recorded as
* an event (OrderBook::groupEvents) and acted on by the broker's trigger pump afterwards, under
* the same rule as every other trigger: nothing fires from inside matching.
*/
struct OrderGroup {
	enum class Kind { OCO, BRACKET };

	std::string id;
	Kind kind = Kind::OCO;
	std::string agentId;

	/* The bracket's entry; empty for a plain OCO */
	std::string parentId;
	/* The OCO pair: the limit leg resting in the book, the stop leg held by the broker. Empty
	*  until a bracket's entry has filled for the first time. */
	std::string bookLegId;
	std::string heldLegId;

	/* What a bracket's children are made of */
	OrderAction childSide = OrderAction::ASK;
	double takeProfit = 0.0;
	double stopLoss = 0.0;
	TimeInForce childTif = TimeInForce::GTC;

	/* Once the pair has resolved, nothing more is created or grown */
	bool resolved = false;
};

/* Something that happened to a group member while matching was under way */
struct GroupEvent {
	enum class Type { FILL, CANCEL };
	Type type;
	std::string groupId;
	std::string orderId;
	unsigned int volume = 0;
};
