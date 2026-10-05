#include "include/Protocol.h"

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
	};
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
	if (type == "order") {
		// Deliberately recognised. Refusing by name is what reserves the shape.
		msg.command = Command::Order;
		msg.error = "order submission is not implemented in protocol version 1";
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
