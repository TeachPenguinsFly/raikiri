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
    [[nodiscard]] net::RestRequest bullet_public() noexcept;

    [[nodiscard]] net::RestRequest contracts_active() noexcept;

    [[nodiscard]] net::RestRequest server_time() noexcept;

    [[nodiscard]] net::RestRequest funding_rate_current(std::string_view symbol) noexcept;

    [[nodiscard]] net::RestRequest level2_snapshot(std::string_view symbol) noexcept;

private:
    std::array<char, 256> target_{};
};

} // namespace raikiri::venues::kucoin
