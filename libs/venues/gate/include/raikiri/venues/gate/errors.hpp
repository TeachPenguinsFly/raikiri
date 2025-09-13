#pragma once

// Gate.io error classification.
//
// The generic gateway retries Retryable, backs off RateLimited, halts on
// AuthFailure and synthesizes a terminal Rejected for Terminal — so a code in
// the wrong bucket is a trading bug, not a logging detail.
//
// Unrecognised labels default to Terminal, not Retryable: an unknown refusal
// repeated at full speed is how an account earns an IP ban, while a
// spuriously terminal order is recoverable by reconciliation.

#include <raikiri/exec/error.hpp>
#include <raikiri/wire/json.hpp>

#include <cstdint>
#include <string_view>

namespace raikiri::venues::gate {

// Every parser in this venue reports failure with this. It lives beside the
// label vocabulary because both answer one question: what did the venue say
// went wrong.
using WireError = wire::Error;

inline constexpr std::string_view k_invalid_param_value = "INVALID_PARAM_VALUE"; // VERIFIED live
inline constexpr std::string_view k_invalid_argument = "INVALID_ARGUMENT";
inline constexpr std::string_view k_invalid_request_body = "INVALID_REQUEST_BODY";
inline constexpr std::string_view k_missing_required_param = "MISSING_REQUIRED_PARAM";
inline constexpr std::string_view k_bad_request = "BAD_REQUEST";
inline constexpr std::string_view k_not_found = "NOT_FOUND";
inline constexpr std::string_view k_too_big = "TOO_BIG"; // VERIFIED live
inline constexpr std::string_view k_authentication_failed = "AUTHENTICATION_FAILED";
inline constexpr std::string_view k_invalid_credentials = "INVALID_CREDENTIALS";
inline constexpr std::string_view k_invalid_key = "INVALID_KEY";
inline constexpr std::string_view k_invalid_signature = "INVALID_SIGNATURE";
inline constexpr std::string_view k_ip_forbidden = "IP_FORBIDDEN";
inline constexpr std::string_view k_read_only = "READ_ONLY";
inline constexpr std::string_view k_forbidden = "FORBIDDEN";
inline constexpr std::string_view k_account_locked = "ACCOUNT_LOCKED";
// VERIFIED live, and misleading: Gate answers a path that does not exist with
// this label too, so a typo'd endpoint presents as missing credentials. It
// still halts the venue, which is the right response to both.
inline constexpr std::string_view k_missing_required_header = "MISSING_REQUIRED_HEADER";
inline constexpr std::string_view k_request_expired = "REQUEST_EXPIRED";
inline constexpr std::string_view k_too_fast = "TOO_FAST";
inline constexpr std::string_view k_too_many_requests = "TOO_MANY_REQUESTS";
inline constexpr std::string_view k_internal = "INTERNAL";
inline constexpr std::string_view k_server_error = "SERVER_ERROR";
inline constexpr std::string_view k_too_busy = "TOO_BUSY";
inline constexpr std::string_view k_contract_not_found = "CONTRACT_NOT_FOUND"; // VERIFIED live
inline constexpr std::string_view k_contract_in_delisting = "CONTRACT_IN_DELISTING";
inline constexpr std::string_view k_invalid_currency_pair = "INVALID_CURRENCY_PAIR";
inline constexpr std::string_view k_balance_not_enough = "BALANCE_NOT_ENOUGH";
inline constexpr std::string_view k_futures_balance_not_enough = "FUTURES_BALANCE_NOT_ENOUGH";
inline constexpr std::string_view k_poc_fill_immediately = "POC_FILL_IMMEDIATELY";
inline constexpr std::string_view k_order_poc_immediate = "ORDER_POC_IMMEDIATE";
inline constexpr std::string_view k_order_not_found = "ORDER_NOT_FOUND";
inline constexpr std::string_view k_client_id_not_found = "CLIENT_ID_NOT_FOUND";
inline constexpr std::string_view k_order_closed = "ORDER_CLOSED";
inline constexpr std::string_view k_order_cancelled = "ORDER_CANCELLED";
inline constexpr std::string_view k_order_finished = "ORDER_FINISHED";
inline constexpr std::string_view k_repeated_creation = "REPEATED_CREATION";
inline constexpr std::string_view k_price_too_deviated = "PRICE_TOO_DEVIATED";
inline constexpr std::string_view k_size_too_large = "SIZE_TOO_LARGE";
inline constexpr std::string_view k_size_too_small = "SIZE_TOO_SMALL";
inline constexpr std::string_view k_fok_not_fill = "FOK_NOT_FILL";
inline constexpr std::string_view k_order_fok = "ORDER_FOK";
inline constexpr std::string_view k_invalid_precision = "INVALID_PRECISION";
inline constexpr std::string_view k_invalid_client_order_id = "INVALID_CLIENT_ORDER_ID";
inline constexpr std::string_view k_order_exists = "ORDER_EXISTS";
inline constexpr std::string_view k_duplicate_request = "DUPLICATE_REQUEST";
inline constexpr std::string_view k_order_not_owned = "ORDER_NOT_OWNED";
inline constexpr std::string_view k_too_many_orders = "TOO_MANY_ORDERS";
inline constexpr std::string_view k_amount_too_little = "AMOUNT_TOO_LITTLE";
inline constexpr std::string_view k_amount_too_much = "AMOUNT_TOO_MUCH";
inline constexpr std::string_view k_no_change = "NO_CHANGE";
inline constexpr std::string_view k_cancel_fail = "CANCEL_FAIL";
// Trading is switched off for this key or account: no amount of retrying
// turns it back on, and a human has to look.
inline constexpr std::string_view k_trade_restricted = "TRADE_RESTRICTED";
// The order exists and the account is fine — the venue is mid-operation on
// one of them, so the same request works a moment later.
inline constexpr std::string_view k_order_pending = "ORDER_PENDING";
inline constexpr std::string_view k_position_in_liquidation = "POSITION_IN_LIQUIDATION";
inline constexpr std::string_view k_position_in_close = "POSITION_IN_CLOSE";

[[nodiscard]] exec::ErrorClass classify(std::string_view label) noexcept;

// Websocket refusal codes; 2 was observed live (unknown contract). There is no
// zero-is-success convention: a code only exists inside an `error` object.
inline constexpr std::int64_t k_ws_invalid_body = 1;
inline constexpr std::int64_t k_ws_invalid_argument = 2; // VERIFIED live
inline constexpr std::int64_t k_ws_server_error = 3;
inline constexpr std::int64_t k_ws_auth_failed = 4;
inline constexpr std::int64_t k_ws_not_logged_in = 6;
inline constexpr std::int64_t k_ws_invalid_key = 11;

[[nodiscard]] exec::ErrorClass classify_stream(std::int64_t code) noexcept;

// A cancel for an order Gate has already finished with usually means it filled
// or was already cancelled; reconciliation settles the truth. Callers use this
// to avoid an alarming CancelRejected for a benign race.
[[nodiscard]] bool is_benign_cancel_failure(std::string_view label) noexcept;

} // namespace raikiri::venues::gate
