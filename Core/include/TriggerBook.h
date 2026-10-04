#pragma once
#include <map>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>

/* What kind of held instruction a trigger stands for */
enum class TriggerKind {
	/* A margin account's liquidation price (OrderModelPlan Step 1.2) */
	MARGIN,
	/* Stops, trailing stops and contingent legs arrive in Phase 2 */
	STOP,
};

/* One price the broker is watching on someone's behalf */
struct TriggerEntry {
	TriggerKind kind;
	std::string agentId;
	/* The held order this stands for, empty for a margin trigger */
	std::string orderId;
	/* Fire on a print AT the price, or only one strictly through it. A margin call is strict
	*  (equity exactly at the requirement is not in violation); a stop is inclusive. */
	bool inclusive;
	/* Placement order, the tiebreak between triggers at the same price */
	unsigned long long seq;
};

/* ---- Trigger book ----
*
* Every price the broker is watching, indexed so a print only has to look at the triggers it
* actually crossed: O(log n + fired), never O(accounts). At 50,000 agents a scan per print
* would dominate the run.
*
* Two sides, by which way the price has to move to fire them:
*
*   SELL triggers fire as the price FALLS to them -- a long margin call, a sell stop.
*        Ordered highest first, the order a falling price reaches them in.
*   BUY  triggers fire as the price RISES to them -- a short margin call, a buy stop.
*        Ordered lowest first.
*
* The book never fires anything itself. collect() hands back what a range of prints crossed,
* removed from the book, and the broker acts on them after matching has returned.
*/
class TriggerBook {
public:
	enum class Side { SELL, BUY };

	/* Set, move or clear an account's margin trigger. A non-positive price clears it. */
	void setMargin(const std::string& agentId, Side side, double price);
	void clearMargin(const std::string& agentId);
	/* Is this account's margin trigger in the book, and where */
	bool marginPrice(const std::string& agentId, double& price) const;

	/* Set, move or clear a held order's trigger (a stop, a trailing stop). Inclusive: a stop
	*  fires on a print AT its price as well as through it. */
	void setOrder(const std::string& orderId, const std::string& agentId, Side side, double price);
	void clearOrder(const std::string& orderId);
	bool orderPrice(const std::string& orderId, double& price) const;

	/* Remove and return every trigger the prints in [low, high] crossed, in firing order:
	*  sell triggers highest first, then buy triggers lowest first */
	std::vector<TriggerEntry> collect(double low, double high);

	bool empty() const { return this->sells.empty() && this->buys.empty(); }
	size_t size() const { return this->sells.size() + this->buys.size(); }
	void clear();

private:
	using SellMap = std::multimap<double, TriggerEntry, std::greater<double>>;
	using BuyMap = std::multimap<double, TriggerEntry>;
	SellMap sells;
	BuyMap buys;

	/* Where each account's margin trigger sits, so moving one is O(log n) */
	struct MarginSlot { Side side; SellMap::iterator sell; BuyMap::iterator buy; };
	std::unordered_map<std::string, MarginSlot> margin;
	/* Where each held order's trigger sits */
	std::unordered_map<std::string, MarginSlot> orders;

	unsigned long long nextSeq = 1;
};
