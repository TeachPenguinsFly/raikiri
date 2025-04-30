#pragma once

// Bitfinex v2 REST request building.

#include <raikiri/net/rest_client.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace raikiri::venues::bitfinex {

[[nodiscard]] std::string_view rest_host() noexcept;

class RequestBuilder {
public:
    // /v2/conf/pub:info:pair — 182 spot rows, ~15 KiB.
    // /v2/conf/pub:info:pair:futures — 90 perpetual rows.
    [[nodiscard]] net::RestRequest pair_info(bool futures) noexcept;

    // /v2/platform/status — `[1]` operative, `[0]` maintenance. Checked before
    // the catalogue load: during maintenance the /conf endpoints still answer,
    // so a gateway would otherwise come up fully populated and permanently
    // silent.
    [[nodiscard]] net::RestRequest platform_status() noexcept;

    // /v2/tickers?symbols=ALL — one row per instrument, the same positional
    // ticker payload the websocket pushes.
    [[nodiscard]] net::RestRequest tickers() noexcept;

    // /v2/book/{symbol}/P0 — the snapshot a depth subscription would start
    // from if anything consumed a book. `len` is constrained by the venue to
    // 1, 25 or 100; anything else answers ["error",10020,"len: invalid"]
    // (verified).
    [[nodiscard]] net::RestRequest book(std::string_view symbol, std::uint32_t len) noexcept;

private:
    std::array<char, 256> target_{};
};

} // namespace raikiri::venues::bitfinex
