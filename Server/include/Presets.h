#pragma once
#include <string>

#include <nlohmann/json.hpp>

#include "FeeSchedule.h"

/* ---- Broker presets (OrderModelPlan Step 4.2) ----
*
* The user's broker, as data: Server/presets/brokers.json, compiled in at configure time.
* Adding or correcting a broker is an edit to that file and nothing else.
*/
namespace Presets {

/* Every preset, as the file has it -- what hello sends a client to build its dropdown */
const nlohmann::json& all();

/* The id a run uses when a reset names none */
inline constexpr const char* DEFAULT_ID = "generic-retail";

/* The fee schedule a preset describes, false (with a reason) for an unknown id or a preset
*  the file gets wrong. SEC and TAF are the engine's standing rates when passed through. */
bool feeSchedule(const std::string& id, FeeSchedule& out, std::string& error);

} // namespace Presets
