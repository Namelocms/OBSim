#include "include/Protocol.h"
#include "include/Presets.h"
#include <cmath>

#include <nlohmann/json.hpp>

#include "Enums.h"
#include "MarketCalendar.h"

using json = nlohmann::json;

namespace {

const char* sessionName(Session s) {
	switch (s) {
	case Session::PREMARKET:  return "PREMARKET";
	case Session::REGULAR:    return "REGULAR";
	case Session::AFTERHOURS: return "AFTERHOURS";
	case Session::OVERNIGHT:  return "OVERNIGHT";
	default:                  return "CLOSED";
	}
}

bool sessionFromName(const std::string& name, Session& out) {
	if (name == "PREMARKET") { out = Session::PREMARKET; return true; }
	if (name == "REGULAR") { out = Session::REGULAR; return true; }
	if (name == "AFTERHOURS") { out = Session::AFTERHOURS; return true; }
	if (name == "OVERNIGHT") { out = Session::OVERNIGHT; return true; }
	if (name == "CLOSED") { out = Session::CLOSED; return true; }
	return false;
}

const char* statusName(AgentStatus s) {
	switch (s) {
	case AgentStatus::ACTIVE:   return "ACTIVE";
	case AgentStatus::INACTIVE: return "INACTIVE";
	case AgentStatus::BANKRUPT: return "BANKRUPT";
	case AgentStatus::LEAVING:  return "LEAVING";
	default:                    return "POOLED";
	}
}

const char* typeName(AgentType t) {
	return (t == AgentType::INSTITUTION) ? "INSTITUTION" : "RETAIL";
}

const char* subTypeName(AgentSubType t) {
	switch (t) {
	case AgentSubType::NOISE:    return "NOISE";
	case AgentSubType::MOMENTUM: return "MOMENTUM";
	case AgentSubType::ALGO:     return "ALGO";
	default:                     return "INFORMED";
	}
}

const char* logKindName(LogEntry::Kind k) {
	switch (k) {
	case LogEntry::Kind::FILL:   return "FILL";
	case LogEntry::Kind::PLACE:  return "PLACE";
	case LogEntry::Kind::CANCEL: return "CANCEL";
	case LogEntry::Kind::ALERT:  return "ALERT";
	case LogEntry::Kind::RISK:   return "RISK";
	case LogEntry::Kind::STOP:   return "STOP";
	default:                     return "HOLD";
	}
}

/* One character per print kind, the most repeated field after the side */
const char* printKindCode(PrintKind k) {
	switch (k) {
	case PrintKind::OPEN_CROSS:   return "O";
	case PrintKind::CLOSE_CROSS:  return "C";
	case PrintKind::REOPEN_CROSS: return "R";
	default:                      return "T";
	}
}

/* The order model switches, as the wire names them (OrderModelPlan Step 4.1) */
json featuresJson(const Features& f) {
	return {
		{ "agentReplace",           f.agentReplace },
		{ "adversityFromEntry",     f.adversityFromEntry },
		{ "agentBrackets",          f.agentBrackets },
		{ "agentPostOnly",          f.agentPostOnly },
		{ "fees",                   f.fees.enabled },
		{ "margin",                 f.margin.enabled },
		{ "shorting",               f.shorting.enabled },
		{ "stopsExtendedHours",     f.stops.extendedHours },
		{ "auctions",               f.auctions.enabled },
		{ "regularOnlyAgentOrders", f.auctions.regularOnlyAgentOrders },
		{ "luld",                   f.luld.enabled },
		{ "luldTier",               f.luld.tier },
	};
}

/* A trade print as [epochSec, price, volume, side, kind]
*
* Side is "B" when a buyer crossed and "S" when a seller did -- one character because this
* is the most repeated field on the wire. Kind is "T" for continuous trading, "O" / "C" for
* the opening and closing crosses and "R" for a reopening after a pause; a cross's side is
* the side of its imbalance.
*/
const char* sideName(OrderAction a) { return (a == OrderAction::BID) ? "B" : "S"; }

const char* tifName(TimeInForce t) {
	switch (t) {
	case TimeInForce::DAY: return "DAY";
	case TimeInForce::GTC: return "GTC";
	case TimeInForce::GTD: return "GTD";
	case TimeInForce::IOC: return "IOC";
	case TimeInForce::FOK: return "FOK";
	case TimeInForce::OPG: return "OPG";
	default:               return "CLS";
	}
}

bool tifFromName(const std::string& name, TimeInForce& out) {
	static const std::pair<const char*, TimeInForce> names[] = {
		{ "DAY", TimeInForce::DAY }, { "GTC", TimeInForce::GTC }, { "GTD", TimeInForce::GTD }, { "IOC", TimeInForce::IOC },
		{ "FOK", TimeInForce::FOK }, { "OPG", TimeInForce::OPG }, { "CLS", TimeInForce::CLS },
	};
	for (const auto& n : names) { if (name == n.first) { out = n.second; return true; } }
	return false;
}

const char* commandName(UserCommand::Kind k) {
	switch (k) {
	case UserCommand::Kind::ORDER:     return "order";
	case UserCommand::Kind::OCO:       return "oco";
	case UserCommand::Kind::BRACKET:   return "bracket";
	case UserCommand::Kind::CANCEL:    return "cancel";
	case UserCommand::Kind::REPLACE:   return "replace";
	default:                           return "sentiment";
	}
}

/* The most a single order may be for, so a hostile message cannot ask for 4 billion shares */
constexpr long long MAX_ORDER_SHARES = 10'000'000;
constexpr double MAX_PRICE = 10'000'000.0;
constexpr size_t MAX_ID_LENGTH = 64;

json printTuple(const TradePrint& t) {
	return json::array({
		MarketCalendar::simTimeToEpochSec(t.timeMs),
		t.price,
		t.volume,
		(t.aggressor == OrderAction::BID) ? "B" : "S",
		printKindCode(t.kind),
		});
}

json envelope(const char* type) {
	json j;
	j["v"] = Protocol::VERSION;
	j["type"] = type;
	return j;
}

} // namespace

namespace Protocol {

std::string encodeHello(const SimParams& params, const SimSessionConfig& config) {
	json j = envelope("hello");
	j["protocol"] = VERSION;

	// What a client cannot work out for itself
	j["epochBase"] = MarketCalendar::SIM_EPOCH_BASE_SEC;
	j["calendar"] = {
		{ "premarketMinutes",  MarketCalendar::PREMARKET_MINUTES },
		{ "regularMinutes",    MarketCalendar::REGULAR_MINUTES },
		{ "afterhoursMinutes", MarketCalendar::AFTERHOURS_MINUTES },
		{ "overnightMinutes",  MarketCalendar::OVERNIGHT_MINUTES },
		{ "activeMinutesPerDay", MarketCalendar::ACTIVE_MINUTES_PER_DAY },
		{ "totalMinutesPerDay",  MarketCalendar::TOTAL_MINUTES_PER_DAY },
	};
	j["params"] = {
		{ "seed",              params.seed },
		{ "backDataDays",      params.backDataDays },
		{ "liveStartSession",  sessionName(params.liveStartSession) },
		{ "minLiquidity",      params.minLiquidity },
		{ "agentCount",        params.agentCount },
		{ "shareFloat",        params.shareFloat },
		{ "startPrice",        params.startPrice },
		{ "transientFraction", params.transientFraction },
		{ "features",          featuresJson(params.features) },
		{ "user", {
			{ "enabled", params.user.enabled },
			{ "cash",    params.user.cash },
			{ "scaled",  params.user.scaled },
			{ "preset",  params.userPreset },
		} },
	};
	// The brokers a user account can be priced from, as the data file has them
	j["presets"] = Presets::all();
	j["config"] = {
		{ "framesPerSecond",    config.framesPerSecond },
		{ "agentRowsPerSecond", config.agentRowsPerSecond },
		{ "agentRowCap",        config.agentRowCap },
		{ "bookDepthLevels",    config.bookDepthLevels },
		{ "logLinesPerFrame",   config.logLinesPerFrame },
	};
	// Tuple layouts, so a client can assert it understands this version rather than
	// silently reading the wrong column if one ever changes.
	j["tuples"] = {
		{ "bookLevel", json::array({ "price", "volume", "orders" }) },
		{ "print",     json::array({ "epochSec", "price", "volume", "side", "kind" }) },
		{ "log",       json::array({ "simTimeMs", "kind", "text" }) },
	};
	return j.dump();
}

std::string encodeFrame(const MarketFrame& f) {
	json j = envelope("frame");
	j["seq"] = f.sequence;
	j["simTimeMs"] = f.simTimeMs;
	j["epochSec"] = f.epochSec;

	j["price"] = f.currentPrice;
	j["spread"] = f.spread;
	j["session"] = sessionName(f.session);
	j["sentiment"] = f.marketNeutralSentiment;
	j["shareFloat"] = f.shareFloat;
	j["tickCount"] = f.tickCount;
	j["restingBids"] = f.restingBids;
	j["restingAsks"] = f.restingAsks;

	j["pop"] = {
		{ "residents",  f.residentAgents },
		{ "transients", f.liveTransients },
		{ "total",      f.totalAgents },
		{ "fraction",   f.transientFraction },
	};
	j["clock"] = {
		{ "speed",           f.speedMultiplier },
		{ "paused",          f.paused },
		{ "running",         f.running },
		{ "backDataRunning", f.backDataRunning },
		{ "backDataAborted", f.backDataAborted },
	};

	json bids = json::array();
	for (const BookLevel& l : f.bids) { bids.push_back(json::array({ l.price, l.volume, l.orders })); }
	json asks = json::array();
	for (const BookLevel& l : f.asks) { asks.push_back(json::array({ l.price, l.volume, l.orders })); }
	j["book"] = { { "bids", std::move(bids) }, { "asks", std::move(asks) } };

	json prints = json::array();
	for (const TradePrint& t : f.trades) { prints.push_back(printTuple(t)); }
	j["prints"] = std::move(prints);
	j["printsDropped"] = f.tradesDropped;

	// Market structure, lending, the house and the broker (OrderModelPlan Step 4.1). Always
	// present, small, and a client draws only the parts its switches make meaningful.
	const FrameMarketState& m = f.market;
	j["market"] = {
		{ "luld", {
			{ "active", m.luldActive }, { "lower", m.luldLower }, { "upper", m.luldUpper },
			{ "reference", m.luldReference }, { "limitState", m.limitState } } },
		{ "pause", { { "paused", m.paused }, { "endsMs", m.pauseEndsMs } } },
		{ "ssr", { { "active", m.ssrActive }, { "untilMs", m.ssrUntilMs }, { "referenceClose", m.ssrReferenceClose } } },
		{ "official", { { "open", m.officialOpen }, { "close", m.officialClose }, { "previousClose", m.previousClose } } },
		{ "auction", {
			{ "collecting", m.auctionCollecting }, { "cross", m.auctionIsOpen ? "OPEN" : "CLOSE" },
			{ "price", m.indicativePrice }, { "matched", m.indicativeMatched }, { "imbalance", m.imbalance },
			{ "side", (m.imbalanceSide == OrderAction::BID) ? "B" : "S" }, { "orders", m.auctionOrders } } },
	};
	j["lending"] = {
		{ "supply", f.lending.supply }, { "borrowed", f.lending.borrowed }, { "utilisation", f.lending.utilisation },
		{ "feeRate", f.lending.feeRate }, { "shortInterest", f.lending.shortInterest },
	};
	j["house"] = {
		{ "commissions", f.house.commissions }, { "exchangeFees", f.house.exchangeFees },
		{ "regulatoryFees", f.house.regulatoryFees }, { "marginInterest", f.house.marginInterest },
		{ "borrowFees", f.house.borrowFees }, { "brokerLosses", f.house.brokerLosses },
	};
	j["broker"] = {
		{ "marginCalls", f.broker.marginCalls }, { "writeOffs", f.broker.writeOffs },
		{ "stopsTriggered", f.broker.stopsTriggered }, { "cascades", f.broker.cascades },
		{ "deepestCascade", f.broker.deepestCascade }, { "recalledShares", f.broker.recalledShares },
		{ "buyIns", f.broker.buyIns }, { "brackets", f.broker.brackets },
		{ "tradingPauses", f.broker.tradingPauses }, { "ssrTriggers", f.broker.ssrTriggers },
	};

	json logs = json::array();
	for (const FrameLogLine& l : f.logs) {
		logs.push_back(json::array({ l.simTimeMs, logKindName(l.kind), l.text }));
	}
	j["logs"] = std::move(logs);
	j["logsDropped"] = f.logsDropped;

	// Absent, not empty, when the roster did not change. A client must be able to tell
	// "unchanged, keep what you have" from "the market emptied".
	if (f.agentsIncluded) {
		json rows = json::array();
		for (const FrameAgentRow& r : f.agents) {
			rows.push_back({
				{ "id",        r.id },
				{ "cash",      r.cash },
				{ "holdings",  r.holdings },
				{ "bids",      r.numBids },
				{ "asks",      r.numAsks },
				{ "sentiment", r.sentiment },
				{ "status",    statusName(r.status) },
				{ "type",      typeName(r.type) },
				{ "subType",   subTypeName(r.subType) },
				{ "transient", r.isTransient },
				{ "stranded",  r.isStranded },
				{ "equity",    r.equity },
				{ "short",     r.shortShares },
				{ "borrowed",  r.borrowedShares },
				{ "buyingPower", r.buyingPower },
				{ "margin",    r.marginPrivileges },
				{ "violation", r.inViolation },
				{ "held",      r.heldOrders },
				});
		}
		j["agents"] = { { "rows", std::move(rows) }, { "omitted", f.agentsOmitted } };
	}

	// The user's account, only when the run has one (OrderModelPlan Step 4.2)
	if (f.user.present) {
		const FrameUser& u = f.user;
		json orders = json::array();
		for (const FrameUserOrder& o : u.orders) {
			std::string type = (o.type == OrderType::LIMIT) ? "limit" : "market";
			if (o.held) {
				type = (o.trailAmount > 0.0 || o.trailPercent > 0.0) ? "trailingStop" : (o.type == OrderType::LIMIT ? "stopLimit" : "stop");
			}
			orders.push_back({
				{ "id", o.id }, { "side", sideName(o.side) }, { "type", type }, { "price", o.price },
				{ "stopPrice", o.stopPrice }, { "trailAmount", o.trailAmount }, { "trailPercent", o.trailPercent * 100.0 },
				{ "qty", o.volume }, { "entered", o.entryVolume }, { "tif", tifName(o.tif) },
				{ "state", o.held ? "held" : (o.inAuction ? "auction" : "working") },
				{ "short", o.shortSale }, { "hidden", o.hidden }, { "displayQty", o.displayQty },
				{ "midpointPeg", o.midpointPeg }, { "extendedHours", o.extendedHours }, { "group", o.groupId },
				});
		}
		json fills = json::array();
		for (const FrameUserFill& x : u.fills) {
			fills.push_back({
				{ "simTimeMs", x.timeMs }, { "orderId", x.orderId }, { "side", sideName(x.side) }, { "price", x.price },
				{ "qty", x.volume }, { "fee", x.fee }, { "liquidity", x.auction ? "cross" : (x.maker ? "added" : "removed") },
				{ "short", x.shortSale },
				});
		}
		j["user"] = {
			{ "broker", u.broker }, { "scaled", u.scaled }, { "moneyScale", u.moneyScale },
			{ "cash", u.cash }, { "escrow", u.escrow }, { "equity", u.equity }, { "buyingPower", u.buyingPower },
			{ "debit", u.debit }, { "long", u.longShares }, { "short", u.shortShares }, { "borrowed", u.borrowedShares },
			{ "margin", u.marginPrivileges }, { "violation", u.inViolation },
			{ "position", u.position }, { "averageCost", u.averageCost }, { "realizedPnl", u.realizedPnl },
			{ "unrealizedPnl", u.unrealizedPnl }, { "feesPaid", u.feesPaid },
			{ "orders", std::move(orders) }, { "fills", std::move(fills) }, { "fillsDropped", u.fillsDropped },
		};
	}

	if (f.backDataRunning) {
		const BackDataProgress& p = f.backData;
		j["backData"] = {
			{ "percent",           p.percent },
			{ "session",           sessionName(p.session) },
			{ "dayIndex",          p.dayIndex },
			{ "activeMinutes",     p.activeMinutes },
			{ "totalMinutes",      p.totalMinutes },
			{ "targetMinutes",     p.targetMinutes },
			{ "events",            p.events },
			{ "ticks",             p.ticks },
			{ "extraDays",         p.extraDays },
			{ "transientArrivals", p.transientArrivals },
			{ "transientFraction", p.transientFraction },
			{ "liveTransients",    p.liveTransients },
		};
	}

	return j.dump();
}

std::string encodeBackfill(const TradeBackfill& backfill) {
	json j = envelope("backfill");
	j["tickCountAtCapture"] = backfill.tickCountAtCapture;
	j["truncated"] = backfill.truncated;

	json prints = json::array();
	prints.get_ref<json::array_t&>().reserve(backfill.trades.size());
	for (const TradePrint& t : backfill.trades) { prints.push_back(printTuple(t)); }
	j["prints"] = std::move(prints);
	return j.dump();
}

std::string encodeError(const std::string& message, const std::string& echo) {
	json j = envelope("error");
	j["message"] = message;
	if (!echo.empty()) { j["echo"] = echo; }
	return j.dump();
}

std::string encodeAck(const std::string& what, const std::string& echo) {
	json j = envelope("ack");
	j["of"] = what;
	if (!echo.empty()) { j["echo"] = echo; }
	return j.dump();
}

std::string encodeUserResult(const UserCommandResult& r) {
	json j = envelope("userResult");
	j["command"] = commandName(r.kind);
	j["accepted"] = r.accepted;
	if (!r.orderId.empty()) { j["orderId"] = r.orderId; }
	if (!r.reason.empty()) { j["reason"] = r.reason; }
	j["simTimeMs"] = r.atMs;
	if (!r.requestId.empty()) { j["echo"] = r.requestId; }
	return j.dump();
}

ControlMessage decodeControl(const std::string& text) {
	ControlMessage msg;

	// Never throws. A frontend bug, a truncated send, or someone poking the socket with
	// curl must produce an error message, not take the server down.
	json j = json::parse(text, nullptr, false);
	if (j.is_discarded() || !j.is_object()) {
		msg.error = "message is not a JSON object";
		return msg;
	}

	if (j.contains("id") && j["id"].is_string()) { msg.id = j["id"].get<std::string>(); }

	if (!j.contains("type") || !j["type"].is_string()) {
		msg.error = "missing type";
		return msg;
	}
	const std::string type = j["type"].get<std::string>();

	auto number = [&j](const char* key, double& out) {
		if (!j.contains(key) || !j[key].is_number()) { return false; }
		out = j[key].get<double>();
		return true;
		};

	if (type == "pause") { msg.command = Command::Pause; msg.valid = true; return msg; }
	if (type == "resume") { msg.command = Command::Resume; msg.valid = true; return msg; }
	if (type == "toggle") { msg.command = Command::Toggle; msg.valid = true; return msg; }
	if (type == "step") { msg.command = Command::Step; msg.valid = true; return msg; }
	if (type == "cancelBackData") { msg.command = Command::CancelBackData; msg.valid = true; return msg; }

	if (type == "speed") {
		msg.command = Command::Speed;
		if (!number("value", msg.value)) { msg.error = "speed needs a numeric value"; return msg; }
		if (!(msg.value > 0.0)) { msg.error = "speed must be positive"; return msg; }
		msg.valid = true;
		return msg;
	}
	if (type == "sentiment") {
		msg.command = Command::Sentiment;
		if (!number("value", msg.value)) { msg.error = "sentiment needs a numeric value"; return msg; }
		msg.valid = true;
		return msg;
	}
	if (type == "sentimentNudge") {
		msg.command = Command::SentimentNudge;
		if (!number("delta", msg.value)) { msg.error = "sentimentNudge needs a numeric delta"; return msg; }
		msg.valid = true;
		return msg;
	}
	if (type == "backfill") {
		msg.command = Command::Backfill;
		double limit = 0.0;
		msg.limit = number("limit", limit) && limit > 0.0 ? (size_t)limit : 100000;
		msg.valid = true;
		return msg;
	}
	// ---- The user's orders (OrderModelPlan Step 4.2) ----
	//
	// Input nobody here produced: every field is type checked and bounded, and a refusal says
	// which field and why. What the broker decides -- buying power, shares to sell, a locate --
	// is the broker's to refuse, after this, as a userResult.
	auto positive = [&j](const char* key, double& out, std::string& error) {
		if (!j.contains(key)) { error = std::string(key) + " is required"; return false; }
		if (!j[key].is_number()) { error = std::string(key) + " must be a number"; return false; }
		double v = j[key].get<double>();
		if (!std::isfinite(v) || !(v > 0.0) || v > MAX_PRICE) { error = std::string(key) + " must be a positive price"; return false; }
		out = v;
		return true;
	};
	auto flag = [](const json& o, const char* key, bool& out, std::string& error) {
		if (!o.contains(key)) { return true; }
		if (!o[key].is_boolean()) { error = std::string(key) + " must be true or false"; return false; }
		out = o[key].get<bool>();
		return true;
	};
	auto shares = [](const json& o, const char* key, long long minimum, unsigned int& out, std::string& error) {
		if (!o.contains(key)) { error = std::string(key) + " is required"; return false; }
		if (!o[key].is_number_integer()) { error = std::string(key) + " must be a whole number of shares"; return false; }
		long long v = o[key].get<long long>();
		if (v < minimum || v > MAX_ORDER_SHARES) { error = std::string(key) + " must be between " + std::to_string(minimum) + " and 10,000,000"; return false; }
		out = (unsigned int)v;
		return true;
	};
	auto orderId = [&j](std::string& out, std::string& error) {
		if (!j.contains("orderId") || !j["orderId"].is_string()) { error = "orderId must be a string"; return false; }
		out = j["orderId"].get<std::string>();
		if (out.empty() || out.size() > MAX_ID_LENGTH) { error = "orderId is empty or too long"; return false; }
		return true;
	};

	if (type == "order") {
		msg.command = Command::Order;
		UserCommand& c = msg.userCommand;
		c.requestId = msg.id;
		std::string& e = msg.error;

		if (!j.contains("side") || !j["side"].is_string()) { e = "side must be buy, sell or sellShort"; return msg; }
		const std::string side = j["side"].get<std::string>();
		OrderAction action = OrderAction::BID;
		SaleMark mark = SaleMark::LONG;
		if (side == "buy") { action = OrderAction::BID; }
		else if (side == "sell") { action = OrderAction::ASK; }
		else if (side == "sellShort") { action = OrderAction::ASK; mark = SaleMark::SHORT; }
		else { e = "side must be buy, sell or sellShort"; return msg; }

		if (!j.contains("orderType") || !j["orderType"].is_string()) { e = "orderType must be market, limit, stop, stopLimit or trailingStop"; return msg; }
		const std::string kind = j["orderType"].get<std::string>();
		const bool isLimit = (kind == "limit" || kind == "stopLimit");
		const bool isStop = (kind == "stop" || kind == "stopLimit");
		const bool isTrail = (kind == "trailingStop");
		if (kind != "market" && !isLimit && !isStop && !isTrail) { e = "orderType must be market, limit, stop, stopLimit or trailingStop"; return msg; }

		OrderRequest r{ action, isLimit ? OrderType::LIMIT : OrderType::MARKET };
		r.mark = mark;
		if (!shares(j, "qty", 1, r.volume, e)) { return msg; }
		if (isLimit && !positive("limitPrice", r.price, e)) { return msg; }
		if (isStop && !positive("stopPrice", r.stopPrice, e)) { return msg; }
		if (isTrail) {
			const bool amount = j.contains("trailAmount"), percent = j.contains("trailPercent");
			if (amount == percent) { e = "a trailing stop needs exactly one of trailAmount and trailPercent"; return msg; }
			if (amount && !positive("trailAmount", r.trailAmount, e)) { return msg; }
			if (percent) {
				if (!j["trailPercent"].is_number()) { e = "trailPercent must be a number"; return msg; }
				double pct = j["trailPercent"].get<double>();
				if (!std::isfinite(pct) || !(pct > 0.0) || !(pct < 100.0)) { e = "trailPercent must be between 0 and 100"; return msg; }
				r.trailPercent = pct / 100.0;
			}
		}

		r.tif = TimeInForce::DAY;
		if (j.contains("tif")) {
			if (!j["tif"].is_string() || !tifFromName(j["tif"].get<std::string>(), r.tif)) { e = "tif must be DAY, GTC, GTD, IOC, FOK, OPG or CLS"; return msg; }
		}
		if (r.tif == TimeInForce::GTD) {
			if (!j.contains("expiresAtMs") || !j["expiresAtMs"].is_number()) { e = "a GTD order needs expiresAtMs"; return msg; }
			r.expiresAtMs = j["expiresAtMs"].get<double>();
			if (!std::isfinite(r.expiresAtMs) || r.expiresAtMs < 0.0) { e = "expiresAtMs must be a sim time"; return msg; }
		}
		bool extended = false;
		if (!flag(j, "extendedHours", extended, e)) { return msg; }
		r.sessions = extended ? SESSIONS_EXTENDED_HOURS : SESSIONS_REGULAR_ONLY;
		if (!flag(j, "postOnly", r.postOnly, e) || !flag(j, "hidden", r.hidden, e) || !flag(j, "midpointPeg", r.midpointPeg, e)) { return msg; }
		if (j.contains("displayQty") && !shares(j, "displayQty", 0, r.displayQty, e)) { return msg; }
		if (r.displayQty >= r.volume) { r.displayQty = 0; }   // showing all of it is not a reserve order

		const bool hasBracket = j.contains("bracket"), hasOco = j.contains("oco");
		if (hasBracket && hasOco) { e = "an order takes a bracket or an OCO, not both"; return msg; }
		if (hasBracket) {
			const json& b = j["bracket"];
			if (!b.is_object()) { e = "bracket must be an object"; return msg; }
			if (isStop || isTrail) { e = "a bracket's entry cannot be a stop"; return msg; }
			if (side == "sell") { e = "a bracket opens a position: buy, or sellShort"; return msg; }
			auto bpos = [&b](const char* key, double& out, std::string& error) {
				if (!b.contains(key) || !b[key].is_number()) { error = std::string("bracket.") + key + " must be a number"; return false; }
				double v = b[key].get<double>();
				if (!std::isfinite(v) || !(v > 0.0) || v > MAX_PRICE) { error = std::string("bracket.") + key + " must be a positive price"; return false; }
				out = v;
				return true;
			};
			c.kind = UserCommand::Kind::BRACKET;
			c.bracket.entry = r;
			if (!bpos("takeProfit", c.bracket.takeProfit, e) || !bpos("stopLoss", c.bracket.stopLoss, e)) { return msg; }
			c.bracket.childTif = TimeInForce::GTC;
		}
		else if (hasOco) {
			const json& o = j["oco"];
			if (!o.is_object()) { e = "oco must be an object"; return msg; }
			if (kind != "limit") { e = "an OCO pairs a limit order with a stop: orderType must be limit"; return msg; }
			OrderRequest stop{ action, OrderType::MARKET };
			stop.mark = mark;
			stop.volume = r.volume;
			stop.tif = (r.tif == TimeInForce::DAY) ? TimeInForce::DAY : TimeInForce::GTC;
			stop.sessions = r.sessions;
			if (!o.contains("stopPrice") || !o["stopPrice"].is_number()) { e = "oco.stopPrice must be a number"; return msg; }
			stop.stopPrice = o["stopPrice"].get<double>();
			if (!std::isfinite(stop.stopPrice) || !(stop.stopPrice > 0.0) || stop.stopPrice > MAX_PRICE) { e = "oco.stopPrice must be a positive price"; return msg; }
			if (o.contains("stopLimitPrice")) {
				if (!o["stopLimitPrice"].is_number()) { e = "oco.stopLimitPrice must be a number"; return msg; }
				double lp = o["stopLimitPrice"].get<double>();
				if (!std::isfinite(lp) || !(lp > 0.0) || lp > MAX_PRICE) { e = "oco.stopLimitPrice must be a positive price"; return msg; }
				stop.type = OrderType::LIMIT;
				stop.price = lp;
			}
			c.kind = UserCommand::Kind::OCO;
			c.order = r;
			c.stopLeg = stop;
		}
		else {
			c.kind = UserCommand::Kind::ORDER;
			c.order = r;
		}
		msg.valid = true;
		return msg;
	}
	if (type == "cancel") {
		msg.command = Command::Cancel;
		msg.userCommand.kind = UserCommand::Kind::CANCEL;
		msg.userCommand.requestId = msg.id;
		if (!orderId(msg.userCommand.orderId, msg.error)) { return msg; }
		msg.valid = true;
		return msg;
	}
	if (type == "replace") {
		msg.command = Command::Replace;
		UserCommand& c = msg.userCommand;
		c.kind = UserCommand::Kind::REPLACE;
		c.requestId = msg.id;
		if (!orderId(c.orderId, msg.error)) { return msg; }
		if (!positive("limitPrice", c.price, msg.error)) { return msg; }
		if (!shares(j, "qty", 1, c.volume, msg.error)) { return msg; }
		msg.valid = true;
		return msg;
	}
	if (type == "reset") {
		msg.command = Command::Reset;
		if (!j.contains("params") || !j["params"].is_object()) {
			msg.error = "reset needs a params object";
			return msg;
		}
		const json& p = j["params"];
		SimParams out;   // defaults stand in for anything not supplied
		// A user account by default, on the generic preset: the person at the screen trades
		out.user.enabled = true;
		out.userPreset = Presets::DEFAULT_ID;
		{
			std::string ignored;
			Presets::feeSchedule(out.userPreset, out.user.fees, ignored);
		}
		auto uintField = [&p](const char* key, unsigned int& dst) {
			if (p.contains(key) && p[key].is_number()) { dst = (unsigned int)p[key].get<double>(); }
			};
		uintField("seed", out.seed);
		uintField("backDataDays", out.backDataDays);
		uintField("minLiquidity", out.minLiquidity);
		uintField("agentCount", out.agentCount);
		uintField("shareFloat", out.shareFloat);
		if (p.contains("startPrice") && p["startPrice"].is_number()) {
			out.startPrice = p["startPrice"].get<double>();
		}
		if (p.contains("transientFraction") && p["transientFraction"].is_number()) {
			out.transientFraction = p["transientFraction"].get<double>();
		}
		if (p.contains("liveStartSession") && p["liveStartSession"].is_string()) {
			if (!sessionFromName(p["liveStartSession"].get<std::string>(), out.liveStartSession)) {
				msg.error = "unknown liveStartSession";
				return msg;
			}
		}
		// The order model switches. Unknown keys are ignored, so a newer client can talk to
		// an older server; a known key of the wrong type is an error, not a silent default.
		if (p.contains("features")) {
			const json& fj = p["features"];
			if (!fj.is_object()) { msg.error = "features must be an object"; return msg; }
			Features& f = out.features;
			struct Flag { const char* key; bool* target; };
			const Flag flags[] = {
				{ "agentReplace", &f.agentReplace }, { "adversityFromEntry", &f.adversityFromEntry },
				{ "agentBrackets", &f.agentBrackets }, { "agentPostOnly", &f.agentPostOnly },
				{ "fees", &f.fees.enabled }, { "margin", &f.margin.enabled }, { "shorting", &f.shorting.enabled },
				{ "stopsExtendedHours", &f.stops.extendedHours }, { "auctions", &f.auctions.enabled },
				{ "regularOnlyAgentOrders", &f.auctions.regularOnlyAgentOrders }, { "luld", &f.luld.enabled },
			};
			for (const Flag& flag : flags) {
				if (!fj.contains(flag.key)) { continue; }
				if (!fj[flag.key].is_boolean()) { msg.error = std::string("features.") + flag.key + " must be true or false"; return msg; }
				*flag.target = fj[flag.key].get<bool>();
			}
			if (fj.contains("luldTier")) {
				if (!fj["luldTier"].is_number_integer()) { msg.error = "features.luldTier must be 1 or 2"; return msg; }
				// Read wide: get<int> would truncate 4294967297 to 1 and accept it
				long long tier = fj["luldTier"].get<long long>();
				if (tier != 1 && tier != 2) { msg.error = "features.luldTier must be 1 or 2"; return msg; }
				f.luld.tier = (int)tier;
			}
		}
		// The user's account (OrderModelPlan Step 4.2)
		if (p.contains("user")) {
			const json& u = p["user"];
			if (!u.is_object()) { msg.error = "user must be an object"; return msg; }
			if (!flag(u, "enabled", out.user.enabled, msg.error)) { msg.error = "user." + msg.error; return msg; }
			if (!flag(u, "scaled", out.user.scaled, msg.error)) { msg.error = "user." + msg.error; return msg; }
			if (u.contains("cash")) {
				if (!u["cash"].is_number()) { msg.error = "user.cash must be a number"; return msg; }
				double cash = u["cash"].get<double>();
				if (!std::isfinite(cash) || cash < 0.0 || cash > 1e12) { msg.error = "user.cash must be between 0 and 1e12"; return msg; }
				out.user.cash = cash;
			}
			if (u.contains("preset")) {
				if (!u["preset"].is_string()) { msg.error = "user.preset must be a preset id"; return msg; }
				std::string why;
				std::string id = u["preset"].get<std::string>();
				if (id.size() > MAX_ID_LENGTH || !Presets::feeSchedule(id, out.user.fees, why)) { msg.error = "user.preset: " + why; return msg; }
				out.userPreset = id;
			}
		}
		if (!(out.startPrice > 0.0)) { msg.error = "startPrice must be positive"; return msg; }
		if (out.shareFloat == 0) { msg.error = "shareFloat must be non-zero"; return msg; }
		if (out.transientFraction < 0.0) { msg.error = "transientFraction cannot be negative"; return msg; }

		msg.params = out;
		msg.valid = true;
		return msg;
	}

	msg.error = "unknown type '" + type + "'";
	return msg;
}

} // namespace Protocol
