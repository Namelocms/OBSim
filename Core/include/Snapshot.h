#pragma once
#include <vector>
#include <memory>

class Order;

/* The book as it stands right now: the touch, the spread, and depth either side.
*
* Deliberately holds NO indicators. macd/rsi/vwap/sma sat here stubbed at 0.00 and are
* gone: an indicator computed in the engine is computed once, at one resolution, for one
* consumer, and a custom one could not be expressed at all. They belong to whoever is
* drawing, derived from the trade stream in OrderBook::tickHistory at whatever resolution
* was actually asked for.
*/
struct Snapshot {
	Snapshot() = default;
	double currentPrice = 0.00;
	double spread = 0.00;
	std::vector<std::shared_ptr<Order>> bids = {};
	std::vector<std::shared_ptr<Order>> asks = {};
};