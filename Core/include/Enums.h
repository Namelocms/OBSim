#pragma once
#include <unordered_map>
#include <string>

enum class OrderStatus { OPEN, CLOSED, CANCELED };
enum class OrderAction { BID, ASK, HOLD, CANCEL };
enum class OrderType { MARKET, LIMIT };
/* How long an order lives, and whether it may rest at all
*
* DAY  -- until the close of the last session it may trade in, on the day it was placed
* GTC  -- good till cancelled, capped at GTC_MAX_DAYS the way brokers cap it
* GTD  -- until an explicit expiresAtMs
* IOC  -- immediate or cancel: match what it can on arrival, cancel the rest. Every market
*         order is IOC
* FOK  -- fill or kill: fills completely on arrival or not at all, and touches nothing if not
*/
enum class TimeInForce { DAY, GTC, GTD, IOC, FOK };
enum class ID_TYPE { ORDER, AGENT };
enum class Session { PREMARKET, REGULAR, AFTERHOURS, OVERNIGHT, CLOSED };
/* Which sessions an order may trade in, one bit per tradeable Session
*
* A real broker distinguishes a regular-session order from an extended-hours one: the first
* does not trade premarket or afterhours even while it is resting. An order outside its
* sessions stays in the book but is skipped by matching, and an incoming one rests without
* matching until its session comes round.
*/
using SessionMask = unsigned char;
inline constexpr SessionMask sessionBit(Session session) {
	switch (session) {
	case Session::PREMARKET:  return 0x01;
	case Session::REGULAR:    return 0x02;
	case Session::AFTERHOURS: return 0x04;
	case Session::OVERNIGHT:  return 0x08;
	default:                  return 0x00;
	}
}
/* Every session. What every agent order uses, which is exactly how the book behaved before
*  orders carried a mask at all. */
inline constexpr SessionMask SESSIONS_ALL = 0x0F;
inline constexpr SessionMask SESSIONS_REGULAR_ONLY = 0x02;
/* A broker's "extended hours" order: premarket through the afterhours close */
inline constexpr SessionMask SESSIONS_EXTENDED_HOURS = 0x07;
/* ACTIVE/INACTIVE are SESSION states, reassigned on every participation sweep.
*  BANKRUPT/LEAVING/POOLED are LIFECYCLE states and must never be overwritten by one. */
enum class AgentStatus { ACTIVE, INACTIVE, BANKRUPT, LEAVING, POOLED };
enum class AgentType { RETAIL, INSTITUTION };
enum class AgentSubType { NOISE, MOMENTUM, ALGO, INFORMED };

/* Is this a lifecycle state rather than a session state?
*
* sweepParticipation reassigns ACTIVE/INACTIVE on every pass. Anything that answers true
* here is off that track: a bankrupt agent, an agent unwinding on its way out, or a pooled
* slot waiting to be rerolled. Overwriting one of these with a session state is how a
* pooled slot silently rejoins the market.
*/
inline bool isLifecycleStatus(AgentStatus status) {
	return status == AgentStatus::BANKRUPT
		|| status == AgentStatus::LEAVING
		|| status == AgentStatus::POOLED;
}

class EnumStrings {
public:
	std::unordered_map<OrderStatus, std::string> orderStatusString = {
		{OrderStatus::OPEN, "OPEN"},
		{OrderStatus::CLOSED, "CLOSED"},
		{OrderStatus::CANCELED, "CANCELED"}
	};
	std::unordered_map<OrderAction, std::string> orderActionString = {
		{OrderAction::BID, "BID"},
		{OrderAction::ASK, "ASK"},
		{OrderAction::HOLD, "HOLD"},
		{OrderAction::CANCEL, "CANCEL"}
	};
	std::unordered_map<OrderType, std::string> orderTypeString = {
		{OrderType::MARKET, "MARKET"},
		{OrderType::LIMIT, "LIMIT"}
	};
	std::unordered_map<TimeInForce, std::string> timeInForceString = {
		{TimeInForce::DAY, "DAY"},
		{TimeInForce::GTC, "GTC"},
		{TimeInForce::GTD, "GTD"},
		{TimeInForce::IOC, "IOC"},
		{TimeInForce::FOK, "FOK"}
	};
	std::unordered_map<ID_TYPE, std::string> idTypeString = {
		{ID_TYPE::ORDER, "ORDER"},
		{ID_TYPE::AGENT, "AGENT"}
	};
	std::unordered_map<Session, std::string> sessionString = {
		{Session::PREMARKET, "PREMARKET"},
		{Session::REGULAR, "REGULAR"},
		{Session::AFTERHOURS, "AFTERHOURS"},
		{Session::OVERNIGHT, "OVERNIGHT"},
		{Session::CLOSED, "CLOSED"}
	};
	std::unordered_map<AgentStatus, std::string> agentStatusString = {
		{AgentStatus::ACTIVE, "ACTIVE"},
		{AgentStatus::INACTIVE, "INACTIVE"},
		{AgentStatus::BANKRUPT, "BANKRUPT"},
		{AgentStatus::LEAVING, "LEAVING"},
		{AgentStatus::POOLED, "POOLED"}
	};
	std::unordered_map<AgentType, std::string> agentTypeString = {
		{AgentType::RETAIL, "RETAIL"},
		{AgentType::INSTITUTION, "INSTITUTION"}
	};
	std::unordered_map<AgentSubType, std::string> agentSubTypeString = {
		{AgentSubType::NOISE, "NOISE"},
		{AgentSubType::MOMENTUM, "MOMENTUM"},
		{AgentSubType::ALGO, "ALGO"},
		{AgentSubType::INFORMED, "INFORMED"},
	};
};