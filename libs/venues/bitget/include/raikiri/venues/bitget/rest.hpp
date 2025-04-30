#pragma once

// Bitget v2 REST request building.

#include <raikiri/net/rest_client.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace raikiri::venues::bitget {

[[nodiscard]] std::string_view rest_host() noexcept;

// Bitget spells one concept twice: this must agree with ws.hpp's instType.
[[nodiscard]] std::string_view product_type(bool testnet) noexcept;

class RequestBuilder {
public:
    // /api/v2/mix/market/contracts — roughly 560 KiB and 745 rows. One
    // productType per call; there is no all-markets variant.
    [[nodiscard]] net::RestRequest contracts(std::string_view product) noexcept;

    // Measures the venue clock offset before anything is signed. The only
    // endpoint here that takes no productType.
    [[nodiscard]] net::RestRequest server_time() noexcept;

    [[nodiscard]] net::RestRequest ticker(std::string_view symbol,
                                          std::string_view product) noexcept;

    [[nodiscard]] net::RestRequest current_fund_rate(std::string_view symbol,
                                                     std::string_view product) noexcept;

    // An out-of-range limit is clamped rather than refused (9999 answers 100
    // levels), so the response never says what it actually gave.
    [[nodiscard]] net::RestRequest
    orderbook(std::string_view symbol, std::string_view product, std::uint32_t depth) noexcept;

private:
    std::array<char, 256> target_{};
};

} // namespace raikiri::venues::bitget
