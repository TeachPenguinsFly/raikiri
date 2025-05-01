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
    [[nodiscard]] net::RestRequest products() noexcept;

    [[nodiscard]] net::RestRequest product(std::string_view product_id) noexcept;

    [[nodiscard]] net::RestRequest server_time() noexcept;

    [[nodiscard]] net::RestRequest book(std::string_view product_id, std::uint32_t level) noexcept;

private:
    std::array<char, 256> target_{};
};

} // namespace raikiri::venues::coinbase
