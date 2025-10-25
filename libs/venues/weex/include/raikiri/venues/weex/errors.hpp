#pragma once

// WEEX error-code classification.
//
// The generic gateway retries Retryable, backs off RateLimited, halts on
// AuthFailure and synthesizes a terminal Rejected for Terminal — so a code in
// the wrong bucket is a trading bug, not a logging detail.
//
// Unrecognised codes default to Terminal, not Retryable: an unknown refusal
// repeated at full speed is how an IP earns a ban, while a spuriously terminal
// read is recoverable by asking again on the next tick.
//
// AuthFailure IS THE WHOLE VENUE. It halts every path at once, so it is
// reserved for a credential that is wrong for every request — a bad key, a bad
// passphrase, a signature outside the clock window. A refusal that is about ONE
// SYMBOL (-1058, -1060, -3235) stays Terminal however much it sounds like a
// permission, because halting a gateway over one unlisted contract silences
// nine hundred that were fine.
//
// The public codes below were read off a live 4xx on 2026-09-07 and the auth
// codes off a live 401 on 2026-09-13; the rest are transcribed from the venue's
// error table and have never been provoked, because provoking them needs a
// credential this machine does not have.

#include <raikiri/exec/error.hpp>
#include <raikiri/wire/error.hpp>

#include <cstdint>

namespace raikiri::venues::weex {

// Every parser in this venue reports failure with this. It lives beside the
// code vocabulary because both answer one question: what did the venue say
// went wrong.
using WireError = wire::Error;

// WEEX codes are negative; zero means no error, which is why a body is judged
// by the SIGN of `code` rather than by the presence of the key.
inline constexpr std::int64_t k_ok = 0;

// --- the credential, for every request alike -----------------------------
inline constexpr std::int64_t k_access_key_empty = -1040;
inline constexpr std::int64_t k_access_sign_empty = -1041;
inline constexpr std::int64_t k_access_timestamp_empty = -1042;
inline constexpr std::int64_t k_access_timestamp_invalid = -1043;
// MEASURED: `GET /capi/v3/account/balance` with no headers answered this at
// HTTP 401 on 2026-09-13, which is what proves the private paths route.
inline constexpr std::int64_t k_access_key_invalid = -1044;
// The 30-second window. A drifting local clock answers here and reads exactly
// like a revoked key, which is why the signed client reads the venue's own
// clock at start.
inline constexpr std::int64_t k_access_timestamp_expired = -1046;
inline constexpr std::int64_t k_auth_failed = -1047;
inline constexpr std::int64_t k_key_or_passphrase_wrong = -1049;
inline constexpr std::int64_t k_user_forbidden = -1050;
inline constexpr std::int64_t k_permission_denied = -1051;
inline constexpr std::int64_t k_insufficient_permissions = -1052;
inline constexpr std::int64_t k_permission_check_failed = -1053;
inline constexpr std::int64_t k_user_auth_not_safe = -1055;
inline constexpr std::int64_t k_illegal_ip = -1056;
inline constexpr std::int64_t k_user_locked = -1057;
inline constexpr std::int64_t k_access_forbidden = -1190;

// --- transient -----------------------------------------------------------
// "System error, please retry later" — the venue's own words, and the only
// code here that earns a retry.
inline constexpr std::int64_t k_system_error = -1054;
inline constexpr std::int64_t k_order_rate_limited = -1059;

// --- the request this process built --------------------------------------
// Both measured: `GET /capi/v3/market/depth` with no symbol answered -1141 and
// with limit=9999 answered -1142, each at HTTP 400.
inline constexpr std::int64_t k_param_empty = -1141;
inline constexpr std::int64_t k_param_invalid = -1142;
// MEASURED: `POST /capi/v3/order` with an empty body answered -1135 at HTTP
// 400 — BEFORE the credential check, which is how an unsigned probe reached it.
inline constexpr std::int64_t k_invalid_json = -1135;

// --- about one order, or one contract ------------------------------------
// A cancel for an order the venue no longer has. BENIGN: the order is already
// gone, which is what the cancel was asking for.
inline constexpr std::int64_t k_order_not_found = -3200;
// Not every listed contract is API-tradable; /capi/v3/market/apiTradingSymbols
// is the venue's own list of the ones that are.
inline constexpr std::int64_t k_symbol_not_api_tradable = -1058;
inline constexpr std::int64_t k_key_not_bound_to_symbol = -1060;
inline constexpr std::int64_t k_contract_no_permission_pair = -3235;

[[nodiscard]] exec::ErrorClass classify(std::int64_t code) noexcept;

// A cancel that failed because there was nothing left to cancel. Reported as
// success, because the caller asked for an order to stop working and it is not
// working — synthesizing a CancelRejected here would leave the OMS holding an
// order the venue does not have.
[[nodiscard]] bool is_benign_cancel_failure(std::int64_t code) noexcept;

} // namespace raikiri::venues::weex
