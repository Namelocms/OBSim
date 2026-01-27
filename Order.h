#pragma once
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>

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
	int volume;
	const int entryVolume;
	std::time_t timestamp;
	OrderStatus status;
	const OrderAction side;
	const OrderType type;
	std::vector<Holding> reservedShares;

	Order() = default;
	Order(
		const std::string id,
		const std::string agentId,
		double price,
		int volume,
		const OrderAction side,
		const OrderType type,
		std::vector<Holding> reservedShares = {}
	);

	/* Get unsold shares from the calling Order */
	std::vector<Holding> getReturnableShares();
	/* Get information about the order as a string */
	std::string toString() const;
};

