#include "include/Order.h"
#include "include/Holding.h"
#include "include/Enums.h"

Order::Order(
	const std::string id,
	const std::string agentId,
	double price,
	int volume,
	const OrderAction side,
	const OrderType type,
	std::vector<Holding> reservedShares
) :
	id(id),
	agentId(agentId),
	price(price),
	volume(volume),
	entryVolume(volume),
	timestamp(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())),
	status(OrderStatus::OPEN),
	side(side),
	type(type),
	reservedShares(reservedShares) {}

std::vector<Holding> Order::getReturnableShares() {
	std::vector<Holding> returnableShares = {};
	int remainingVol = this->volume;
	int useVol = 0;

	for (Holding h : this->reservedShares) {
		if (remainingVol == 0) { break; }
		useVol = std::min(h.volume, remainingVol);
		if (useVol > 0) {
			returnableShares.push_back(Holding(h.price, useVol));
			remainingVol -= useVol;
		}
	}
	return returnableShares;
}
std::string Order::toString() const {
	EnumStrings es;
	std::string reserved_string = "";

	for (Holding h : this->reservedShares) {
		reserved_string += ("______$" + std::to_string(h.price) + ": " + std::to_string(h.volume) + "\n");
	}

	return "__ID: " + this->id +
		"\n____AGENT_ID: " + this->agentId +
		"\n____PRICE: " + std::to_string(this->price) +
		"\n____VOLUME: " + std::to_string(this->volume) +
		"\n____ENTRY_VOLUME: " + std::to_string(this->entryVolume) +
		"\n____TIMESTAMP: " + std::to_string(this->timestamp) +
		"\n____STATUS: " + es.orderStatusString[this->status] +
		"\n____SIDE: " + es.orderActionString[this->side] +
		"\n____TYPE: " + es.orderTypeString[this->type] +
		"\n____RESERVED_SHARES:\n" + reserved_string +
		"\n=======================================\n";
}