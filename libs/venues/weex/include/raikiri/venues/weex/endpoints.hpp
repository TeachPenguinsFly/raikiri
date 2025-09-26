#pragma once

// Every address this adapter speaks to, in one place.
//
// A path spelled inline is a path nobody can audit: when WEEX moves one, or
// when a reader asks "what does this process actually call", the answer has to
// be greppable rather than scattered through a thousand lines of request
// building.
//
// WEEX runs two exchanges under one brand and this adapter reaches only the
// USDT-margined contract tree. Spot lives on other hosts entirely and nothing
// here addresses it. There is no testnet: the documented hosts below are the
// only ones WEEX publishes, so `--testnet` has nowhere else to point.

#include <string_view>

namespace raikiri::venues::weex::ep {

// --- hosts ---------------------------------------------------------------
inline constexpr std::string_view k_rest_host = "api-contract.weex.com";
inline constexpr std::string_view k_stream_host = "ws-contract.weex.com";
inline constexpr std::string_view k_stream_port = "443";

// --- sockets -------------------------------------------------------------
// One public socket for every channel; the private path is /v3/ws/private and
// is not reached from here.
inline constexpr std::string_view k_ws_public = "/v3/ws/public";

// --- reference data ------------------------------------------------------
// `assets`, `rateLimits` and `symbols` in one body; 1022 contracts and 685 KiB
// measured 2026-09-07, which is why it is read once at start and never polled.
inline constexpr std::string_view k_exchange_info = "/capi/v3/market/exchangeInfo";

// Funding for every contract in one answer: lastFundingRate, the forecast for
// the next interval, nextFundingTime and collectCycle. WEEX pushes none of
// this on any socket, so it is polled or it does not arrive at all.
inline constexpr std::string_view k_premium_index = "/capi/v3/market/premiumIndex";

} // namespace raikiri::venues::weex::ep
