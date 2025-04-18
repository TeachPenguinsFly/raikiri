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
    // /quote/contracts — the whole perpetual catalogue, roughly 470 KiB for
    // 1021 rows. An optional `symbol` filters it; nothing here needs that.
    [[nodiscard]] net::RestRequest contracts() noexcept;

    // /server/time — used to measure the venue clock offset before anything is
    // signed.
    [[nodiscard]] net::RestRequest server_time() noexcept;

    // /quote/premiumIndex — mark, index, the rate in force and the next
    // settlement. Scoped to one symbol: unscoped it answers with every
    // contract, ~300 KiB to learn one rate.
    [[nodiscard]] net::RestRequest premium_index(std::string_view symbol) noexcept;

    // /quote/depth — the snapshot a depth subscription would start from if
    // anything consumed a book. `limit` must be one of 5/10/20/50/100/500/1000;
    // anything else is refused with 109400 (VERIFIED).
    [[nodiscard]] net::RestRequest depth(std::string_view symbol, std::uint32_t limit) noexcept;

    [[nodiscard]] net::RestRequest ticker(std::string_view symbol) noexcept;

private:
    std::array<char, 256> target_{};
};

} // namespace raikiri::venues::bingx
