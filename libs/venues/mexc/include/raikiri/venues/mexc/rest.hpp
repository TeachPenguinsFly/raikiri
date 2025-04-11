#pragma once

// MEXC futures REST request building.
//
// Only the unauthenticated reads the market-data path needs. Signing arrives
// with order entry, and its shape is already decided — MEXC signs
// apiKey + timestamp + (query string or raw JSON body) with HMAC-SHA256, hex
// encodes the digest, and carries ApiKey, Request-Time and Signature in
// HEADERS rather than in the query. Recording it here keeps the next commit
// from rediscovering it.
//
// Two path shapes coexist and mixing them up costs a debug cycle: depth and
// funding_rate take the symbol as a PATH SEGMENT, while ticker takes it as a
// query parameter.
//
// There is no futures testnet host, so nothing here takes a flag.

#include <raikiri/net/rest_client.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace raikiri::venues::mexc {

[[nodiscard]] std::string_view rest_host() noexcept;

class RequestBuilder {
public:
    // /api/v1/contract/detail — the whole futures catalogue in one call,
    // roughly 2 MiB for 1086 rows. There is no per-symbol or per-market
    // variant and no query parameter to narrow it.
    [[nodiscard]] net::RestRequest contract_detail() noexcept;

    // /api/v1/contract/ping — the venue clock, despite the name; its `data` is
    // the epoch-millisecond number.
    [[nodiscard]] net::RestRequest server_time() noexcept;

    [[nodiscard]] net::RestRequest depth(std::string_view symbol, std::uint32_t limit) noexcept;

    [[nodiscard]] net::RestRequest ticker(std::string_view symbol) noexcept;

    // The only place the funding SCHEDULE exists: the websocket ticker carries
    // a rate but never a settlement time.
    [[nodiscard]] net::RestRequest funding_rate(std::string_view symbol) noexcept;

private:
    std::array<char, 256> target_{};
};

} // namespace raikiri::venues::mexc
