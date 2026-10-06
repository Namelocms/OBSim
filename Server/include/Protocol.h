#pragma once
#include <string>

#include "MarketFrame.h"
#include "SimSession.h"

/* ---- The wire protocol ----
*
* JSON, because it is debuggable in a browser's network tab and because a third party
* writing a bot against this should be able to read the wire rather than needing a schema.
* If measurement says it saturates, the bulk payloads become binary and the control
* messages stay as they are -- which is why they are separable here.
*
* Every message carries `v`, the protocol version, and `type`. Nothing else is guaranteed
* across versions.
*
* REPEATED DATA IS SENT AS TUPLES, NOT OBJECTS. A book level is [price, volume, orders]
* and a trade print is [epochSec, price, volume, side, kind]. Twenty levels a side at 30 frames
* a second makes the difference between a key name repeated 1,200 times a second and not.
* The orders are fixed and documented here; treat them as part of the contract.
*
* Sim time is sent as BOTH `simTimeMs` and `epochSec`. simTimeMs is authoritative;
* epochSec is a convenience for the chart's axis and is lossy below a microsecond. See
* MarketCalendar::simTimeToEpochSec.
*/

namespace Protocol {

/* Bumped when an existing field changes meaning or disappears. Adding a field does not
*  bump it: a client is expected to ignore what it does not recognise. */
inline constexpr int VERSION = 1;

// ---- Outbound ----

/* Sent once per connection, before anything else
*
* Carries what a client cannot derive: the epoch base, the session lengths, and the run's
* parameters. A client that understands hello can lay out its axis before a frame arrives.
*/
std::string encodeHello(const SimParams& params, const SimSessionConfig& config);

/* One frame. The bulk of the traffic. */
std::string encodeFrame(const MarketFrame& frame);

/* The retained trade history, for a client that joined mid-run */
std::string encodeBackfill(const TradeBackfill& backfill);

/* An error, in reply to a control message that could not be honoured
*
* `echo` is the client's own `id` if it sent one, so a request can be correlated with its
* failure. Empty when the message had none, or could not be parsed far enough to find it.
*/
std::string encodeError(const std::string& message, const std::string& echo);

/* Acknowledgement of a control message that was honoured */
std::string encodeAck(const std::string& what, const std::string& echo);

/* The outcome of one of the user's commands, applied or refused (OrderModelPlan Step 4.2).
*  Sent to every client: they all show the same account. `echo` is the request's id. */
std::string encodeUserResult(const UserCommandResult& result);

// ---- Inbound ----

/* What a control message asked for */
enum class Command {
	Unknown,
	Pause,
	Resume,
	Toggle,
	Step,
	Speed,
	Sentiment,
	SentimentNudge,
	Backfill,
	Reset,
	CancelBackData,
	/* The user's own orders (OrderModelPlan Step 4.2): a new order (with an optional
	*  bracket or OCO), a cancel and a replace. Carried in ControlMessage::userCommand. */
	Order,
	Cancel,
	Replace,
};

struct ControlMessage {
	Command command = Command::Unknown;
	/* Client supplied correlation id, echoed back on ack or error. Optional. */
	std::string id;
	/* Speed multiplier, sentiment value, or sentiment delta, by command */
	double value = 0.0;
	/* Backfill depth in trades */
	size_t limit = 0;
	/* Parameters for Reset */
	SimParams params;
	/* Order, Cancel and Replace */
	UserCommand userCommand;
	/* Set when the message could not be understood; the reason is in `error` */
	bool valid = false;
	std::string error;
};

/* Parse one inbound message. Never throws: a malformed message comes back invalid. */
ControlMessage decodeControl(const std::string& text);

} // namespace Protocol
