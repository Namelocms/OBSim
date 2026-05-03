#pragma once
#include <vector>
#include <memory>

class Order;

struct Snapshot {
	Snapshot() = default;
	double currentPrice = 0.00;
	double macd = 0.00;
	double rsi = 0.00;
	double vwap = 0.00;
	double sma = 0.00;
	double spread = 0.00;
	std::vector<std::shared_ptr<Order>> bids = {};
	std::vector<std::shared_ptr<Order>> asks = {};
};