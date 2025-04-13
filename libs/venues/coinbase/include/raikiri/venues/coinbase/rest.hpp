#pragma once

// Coinbase Exchange REST request building.
//
// Only the unauthenticated reads the market-data path needs, and that is all
// this adapter will ever build: order entry is out of scope here, so unlike
// okx's header there is no signing scheme recorded for a later commit.
//
// This is the EXCHANGE API (api.exchange.coinbase.com), the former Coinbase
// Pro, not the newer Advanced Trade API on api.coinbase.com. They disagree on
// product naming, on field names, and on whether public reads need a key —
// these do not, which is what makes Coinbase usable as a reference venue with
// no credentials on the box.

#include <raikiri/net/rest_client.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace raikiri::venues::coinbase {

[[nodiscard]] std::string_view rest_host() noexcept;

class RequestBuilder {
public:
    // /products — the whole catalogue, ~350 KiB for 832 rows. There is no
    // filter parameter, so spot-only is a property of the venue rather than
    // of the request.
    [[nodiscard]] net::RestRequest products() noexcept;

    [[nodiscard]] net::RestRequest product(std::string_view product_id) noexcept;

    // /time — used to measure the venue clock offset. It answers with an
    // ISO-8601 string beside a floating-point epoch; only the string is read.
    [[nodiscard]] net::RestRequest server_time() noexcept;

    // /products/{id}/book — level 1 is best bid/ask only, level 2 the
    // aggregated book. The snapshot a depth subscription would start from, if
    // anything consumed a book.
    [[nodiscard]] net::RestRequest book(std::string_view product_id, std::uint32_t level) noexcept;

private:
    std::array<char, 256> target_{};
};

} // namespace raikiri::venues::coinbase
