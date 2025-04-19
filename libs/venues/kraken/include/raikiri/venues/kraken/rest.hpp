#pragma once

// Kraken Futures derivatives v3 REST request building.
//
// Only the unauthenticated reads the market-data path needs. This venue is
// read-only by design, so signing is out of scope rather than a TODO, and
// nothing below carries a header slot for it.
//
// There is no clock endpoint: every response carries `serverTime`, and
// `ticker` is the cheapest such read (~600 bytes against ~1 MiB).

#include <raikiri/net/rest_client.hpp>

#include <array>
#include <string_view>

namespace raikiri::venues::kraken {

[[nodiscard]] std::string_view rest_host() noexcept;

class RequestBuilder {
public:
    // Every market in one call, ~1 MiB and 298 rows live. Unlike okx there is
    // no instType to narrow it, so the perp/dated split is the mapper's.
    [[nodiscard]] net::RestRequest instruments() noexcept;

    // /derivatives/api/v3/tickers/{symbol} — one row plus `serverTime`.
    [[nodiscard]] net::RestRequest ticker(std::string_view symbol) noexcept;

    // The snapshot a depth subscription would start from, if anything
    // consumed a book.
    [[nodiscard]] net::RestRequest orderbook(std::string_view symbol) noexcept;

private:
    std::array<char, 256> target_{};
};

} // namespace raikiri::venues::kraken
