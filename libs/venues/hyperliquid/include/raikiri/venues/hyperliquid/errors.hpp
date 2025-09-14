#pragma once

// Hyperliquid error classification.
//
// The generic gateway retries Retryable, backs off RateLimited, halts on
// AuthFailure and synthesizes a terminal Rejected for Terminal, so a status in
// the wrong bucket is a trading bug. Unrecognised failures default to Terminal:
// an unknown refusal repeated at full speed is how an address earns a ban.
//
// Unlike every CEX adapter here there is NO numeric code envelope — /info
// answers with an HTTP status and prose. The one reason `body` is a parameter:
// a query the venue cannot satisfy comes back 200 OK with a body of `null`
// (verified live), so nothing but the body separates it from a real read.

#include <raikiri/exec/error.hpp>
#include <raikiri/wire/json.hpp>

#include <string_view>

namespace raikiri::venues::hyperliquid {

// Every parser in this venue reports failure with this. It lives beside the
// classifier because both answer one question: what did the venue say went
// wrong.
using WireError = wire::Error;

// A body carrying no answer, spelled as the JSON literal `null`.
[[nodiscard]] bool is_null_body(std::string_view body) noexcept;

// Called only once a response has already failed to be read. `body` decides
// where the status cannot: a 2xx whose body is `null` is a refusal wearing a
// success status, while a 2xx whose body merely would not parse is a corrupt
// read worth repeating.
[[nodiscard]] exec::ErrorClass classify(int status, std::string_view body) noexcept;

// Classifies an /exchange refusal, which is PROSE and nothing else — there is
// no numeric code anywhere in this protocol, on either endpoint.
//
// Matched by PREFIX, never by equality: the venue appends context to several of
// these ("Insufficient margin to place order. asset=84", "Post only order would
// have immediately matched, bbo was ..."), and those are the ones a strategy
// meets most often. Anything unrecognised is Terminal, because an unknown
// refusal retried at full speed is how an address earns a rate-limit ban.
[[nodiscard]] exec::ErrorClass classify_reason(std::string_view reason) noexcept;

// A cancel refusal meaning the order was already gone. The cancel did not fail
// so much as arrive late, and reporting it as a failure leaves the OMS holding
// an order the venue has already closed.
[[nodiscard]] bool is_benign_cancel_failure(std::string_view reason) noexcept;

} // namespace raikiri::venues::hyperliquid
