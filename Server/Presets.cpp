#include "include/Presets.h"

#include <cstddef>

// Generated at configure time from presets/brokers.json (Server/CMakeLists.txt)
extern const unsigned char BROKER_PRESETS_JSON[];
extern const std::size_t BROKER_PRESETS_JSON_SIZE;

using json = nlohmann::json;

namespace Presets {

const json& all() {
	// Parsed once. A malformed file is a build mistake, so it shows as an empty list and
	// every preset id is refused by name, rather than taking the server down.
	static const json presets = [] {
		json parsed = json::parse(BROKER_PRESETS_JSON, BROKER_PRESETS_JSON + BROKER_PRESETS_JSON_SIZE, nullptr, false);
		if (parsed.is_discarded() || !parsed.contains("presets") || !parsed["presets"].is_array()) { return json::array(); }
		return parsed["presets"];
	}();
	return presets;
}

static double num(const json& j, const char* key, double fallback = 0.0) {
	return (j.is_object() && j.contains(key) && j[key].is_number()) ? j[key].get<double>() : fallback;
}

bool feeSchedule(const std::string& id, FeeSchedule& out, std::string& error) {
	for (const json& p : all()) {
		if (!p.is_object() || !p.contains("id") || p["id"] != id) { continue; }
		// Regulatory rates and the generic margin terms are the engine's; the preset overrides
		FeeSchedule f = FeeSchedule::zeroCommissionRetail();
		f.name = p.value("name", id);
		f.asOf = p.value("asOf", "");

		const json& c = p.contains("commission") ? p["commission"] : json::object();
		f.commissionPerShare = num(c, "perShare");
		f.commissionPerOrder = num(c, "perOrder");
		f.commissionMin = num(c, "min");
		f.commissionMaxPct = num(c, "maxPct");

		const json& x = p.contains("exchangeFees") ? p["exchangeFees"] : json::object();
		f.passThroughExchangeFees = x.is_object() && x.value("passThrough", false);
		f.takerFeePerShare = num(x, "takerPerShare");
		f.makerRebatePerShare = num(x, "makerRebatePerShare");
		f.subDollarTakerFeePct = num(x, "subDollarTakerPct");
		f.subDollarMakerRebatePct = num(x, "subDollarMakerRebatePct");

		const json& r = p.contains("regulatory") ? p["regulatory"] : json::object();
		if (r.is_object() && !r.value("passThrough", true)) { f.secFeeRate = 0.0; f.tafPerShare = 0.0; f.tafMaxPerTrade = 0.0; }
		f.catPerShare = num(r, "catPerShare");

		const json& m = p.contains("margin") ? p["margin"] : json::object();
		f.marginApr = num(m, "apr", f.marginApr);
		f.houseMaintenance = num(m, "houseMaintenance", f.houseMaintenance);
		f.marginTiersBlended = m.is_object() && m.value("blended", false);
		f.marginInterestFree = num(m, "interestFree");
		if (m.is_object() && m.contains("tiers") && m["tiers"].is_array()) {
			const json& tiers = m["tiers"];
			if (tiers.size() > (size_t)FeeSchedule::MAX_MARGIN_TIERS) { error = "preset " + id + " has too many margin tiers"; return false; }
			double previous = -1.0;
			for (const json& t : tiers) {
				double from = num(t, "from", -1.0), apr = num(t, "apr", -1.0);
				if (from < 0.0 || apr < 0.0 || from <= previous) { error = "preset " + id + " has a malformed margin tier"; return false; }
				f.marginTierFrom[f.marginTierCount] = from;
				f.marginTierApr[f.marginTierCount] = apr;
				++f.marginTierCount;
				previous = from;
			}
			if (f.marginTierCount > 0) { f.marginApr = f.marginTierApr[0]; }
		}
		if (f.commissionPerShare < 0 || f.commissionPerOrder < 0 || f.commissionMin < 0 || f.commissionMaxPct < 0
			|| f.marginApr < 0 || f.houseMaintenance < 0 || f.houseMaintenance >= 1.0 || f.catPerShare < 0) {
			error = "preset " + id + " has a negative or impossible figure";
			return false;
		}
		out = f;
		return true;
	}
	error = "unknown broker preset " + id;
	return false;
}

} // namespace Presets
