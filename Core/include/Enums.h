#pragma once
#include <unordered_map>
#include <string>

enum class OrderStatus { OPEN, CLOSED, CANCELED };
enum class OrderAction { BID, ASK, HOLD, CANCEL };
enum class OrderType { MARKET, LIMIT };
enum class ID_TYPE { ORDER, AGENT };
enum class Session { PREMARKET, REGULAR, AFTERHOURS, OVERNIGHT, CLOSED };
enum class AgentStatus { ACTIVE, INACTIVE, BANKRUPT };
enum class AgentType { RETAIL, INSTITUTION };
enum class AgentSubType { NOISE, MOMENTUM, ALGO, INFORMED };

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
		{AgentStatus::BANKRUPT, "BANKRUPT"}
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