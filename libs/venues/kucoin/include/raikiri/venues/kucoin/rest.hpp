#pragma once

// KuCoin Futures REST request building.
//
// Only the unauthenticated reads the market-data path needs. Signing arrives
// with order entry: HMAC-SHA256 over timestamp + method + requestPath + body,
// base64'd, alongside a PASSPHRASE that is itself HMAC'd under
// KC-API-KEY-VERSION 2 — the part with no binance or bybit analogue, recorded
// here so the next commit does not rediscover it.
//
// There is no testnet: KuCoin retired its futures sandbox, so nothing here
// takes a flag.

#include <raikiri/net/rest_client.hpp>

#include <array>
#include <string_view>

namespace raikiri::venues::kucoin {

[[nodiscard]] std::string_view rest_host() noexcept;

class RequestBuilder {
public:
    // POST /api/v1/bullet-public — unauthenticated, empty body, and the only
    // way to learn a websocket URL. Its token expires, so this runs on every
    // connect rather than once.
    [[nodiscard]] net::RestRequest bullet_public() noexcept;

    // /api/v1/contracts/active — every listed contract in one call, roughly
    // 900 KiB across 677 rows. There is no per-market filter.
    [[nodiscard]] net::RestRequest contracts_active() noexcept;

    // /api/v1/timestamp — used to measure the venue clock offset before
    // anything is signed. KuCoin's signing window is 5 s, the tightest of the
    // venues here.
    [[nodiscard]] net::RestRequest server_time() noexcept;

    // /api/v1/funding-rate/{symbol}/current — seeds funding at subscribe time,
    // because the stream only pushes at a settlement boundary up to eight
    // hours away.
    [[nodiscard]] net::RestRequest funding_rate_current(std::string_view symbol) noexcept;

    // /api/v1/level2/snapshot — the book a depth subscription would start
    // from, since the level2 stream carries deltas only and no snapshot.
    [[nodiscard]] net::RestRequest level2_snapshot(std::string_view symbol) noexcept;

private:
    std::array<char, 256> target_{};
};

} // namespace raikiri::venues::kucoin
