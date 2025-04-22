#pragma once

// BitMEX v1 REST request building.
//
// Only the unauthenticated reads the market-data path needs. Signing arrives
// with order entry and its shape is already decided — HMAC-SHA256 over
// expires + method + path + body, hex, in api-key/api-signature/api-expires —
// where `expires` is an absolute wall-clock DEADLINE in seconds rather than the
// request time every other venue here sends. No clock-sync call exists below
// because nothing is signed yet and BitMEX publishes no server-time endpoint.
//
// The catalogue is ONE request on purpose: the unauthenticated budget is
// per-minute and shared across the endpoint family (x-ratelimit-limit: 180).

#include <raikiri/net/rest_client.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace raikiri::venues::bitmex {

[[nodiscard]] std::string_view rest_host(bool testnet) noexcept;

class RequestBuilder {
public:
    // ~110 KiB, every listed contract in one answer, no market parameter.
    [[nodiscard]] net::RestRequest instruments() noexcept;

    // Single-row form, for confirming one contract without a catalogue pull.
    [[nodiscard]] net::RestRequest instrument(std::string_view symbol) noexcept;

    // NOT /orderBookL2_25: that is a websocket table name and 404s over REST.
    [[nodiscard]] net::RestRequest book(std::string_view symbol, std::uint32_t depth) noexcept;

    [[nodiscard]] net::RestRequest trades(std::string_view symbol, std::uint32_t count) noexcept;

private:
    std::array<char, 256> target_{};
};

} // namespace raikiri::venues::bitmex
