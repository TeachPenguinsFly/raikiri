#pragma once

// Bitget v2 REST request building.
//
// Only the unauthenticated reads the market-data path needs. Signing arrives
// with order entry, and its shape is decided — HMAC-SHA256 over
// timestamp + method + requestPath + body, base64'd, carried in ACCESS-KEY,
// ACCESS-SIGN, ACCESS-TIMESTAMP (epoch milliseconds, not okx's ISO-8601) and
// an ACCESS-PASSPHRASE header.
//
// productType is required on every mix endpoint, including ones that already
// name a symbol, and omitting it does not default: /mix/market/ticker with
// only a symbol answers 400172 "Parameter verification failed", which reads
// like a malformed symbol rather than a missing sibling parameter. There is
// no separate demo host either — productType SUSDT-FUTURES on this same host
// serves the simulated product, with S-prefixed symbols.

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
