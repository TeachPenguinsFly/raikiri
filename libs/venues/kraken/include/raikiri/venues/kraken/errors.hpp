#pragma once

// Kraken Futures error classification.
//
// What differs here: the code is a STRING NAME and there is no numeric code
// anywhere in a Kraken error body. The name carries a suffix naming the
// offending field — VERIFIED, `GET /orderbook?symbol=` answers
// "invalidArgument: symbol" — so classification matches up to the first ':'.
//
// HTTP status is no substitute: an unauthenticated /accounts read returns
// HTTP 200 with "authenticationError" (verified), so the body is the truth.

#include <raikiri/exec/error.hpp>
#include <raikiri/wire/json.hpp>

#include <string_view>

namespace raikiri::venues::kraken {

// Every parser in this venue reports failure with this. It lives beside the
// code vocabulary because both answer one question: what did the venue say
// went wrong.
using WireError = wire::Error;

// Classifies a Kraken Futures error name, with or without its ": field"
// suffix. An empty name is Terminal: a failure we cannot name is not one to
// retry blindly.
[[nodiscard]] exec::ErrorClass classify(std::string_view error) noexcept;

// A cancel for an order Kraken has already finished with usually means the
// order filled or was already cancelled; reconciliation settles the truth.
[[nodiscard]] bool is_benign_cancel_failure(std::string_view error) noexcept;

// The per-order verdict, which is a SECOND vocabulary: the envelope reports
// result "success" for a refused order as readily as for an accepted one, so
// `sendStatus.status` is the only thing that says what happened. Anything the
// three helpers below do not name is a refusal.
[[nodiscard]] bool is_send_accepted(std::string_view status) noexcept;

// "filled" and "notFound" mean the order was already gone; both are answers a
// cancel can live with, and neither is a reason to tell the OMS its cancel
// failed while the order is in fact no longer working.
[[nodiscard]] bool is_cancel_accepted(std::string_view status) noexcept;

[[nodiscard]] bool is_edit_accepted(std::string_view status) noexcept;

} // namespace raikiri::venues::kraken
