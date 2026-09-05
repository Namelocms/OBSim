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
	unsigned int volume;
	const unsigned int entryVolume;
	double timestamp;
	OrderStatus status;
	const OrderAction side;
	const OrderType type;
	std::vector<Holding> reservedShares;
	/* Cash escrowed against this bid at placement time, drawn down as the order fills and refunded when unused */
	double reservedCash;

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
		double reservedCash = 0.0
	);

	/* Get unsold shares from the calling Order */
	std::vector<Holding> getReturnableShares();
	/* Get information about the order as a string */
	std::string toString() const;
};

