#pragma once

// BingX swap v2 REST request building.
//
// Only the unauthenticated reads the market-data path needs. Signing arrives
// with order entry, and the venue already showed its shape by refusing without
// it: an X-BX-APIKEY header (100413 when absent, VERIFIED) plus `timestamp` and
// `signature` in the query.
//
// There is no separate demo host — BingX runs demo trading as VST-quoted
// contracts on this same API — so nothing here takes a testnet flag.

#include <raikiri/net/rest_client.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace raikiri::venues::bingx {

[[nodiscard]] std::string_view rest_host() noexcept;

class RequestBuilder {
public:
    [[nodiscard]] net::RestRequest contracts() noexcept;

    [[nodiscard]] net::RestRequest server_time() noexcept;

    [[nodiscard]] net::RestRequest premium_index(std::string_view symbol) noexcept;

    [[nodiscard]] net::RestRequest depth(std::string_view symbol, std::uint32_t limit) noexcept;

    [[nodiscard]] net::RestRequest ticker(std::string_view symbol) noexcept;

private:
    std::array<char, 256> target_{};
};

} // namespace raikiri::venues::bingx
