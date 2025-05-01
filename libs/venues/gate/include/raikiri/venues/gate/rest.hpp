#pragma once

// Gate.io v4 REST request building.
//
// Only the unauthenticated reads the market-data path needs. Signing arrives
// with order entry, and its shape is already decided — Gate signs
// method + "\n" + path + "\n" + query + "\n" + SHA512(body) + "\n" + timestamp
// with HMAC-SHA512 and carries KEY, SIGN and Timestamp headers. The hashed
// body inside the signed payload, and SHA512 over SHA256, are the parts with
// no binance or okx analogue.
//
// Testnet is a different HOST here (api-testnet.gateapi.io, verified
// answering), not a header the way okx does it.

#include <raikiri/net/rest_client.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace raikiri::venues::gate {

[[nodiscard]] std::string_view rest_host(bool testnet) noexcept;

// USDT-settled perpetuals. Gate keys the whole futures API on the settlement
// currency, so this reaches every path rather than being a query parameter.
inline constexpr std::string_view k_settle_usdt = "usdt";

class RequestBuilder {
public:
    [[nodiscard]] net::RestRequest contracts(std::string_view settle) noexcept;

    [[nodiscard]] net::RestRequest server_time() noexcept;

    [[nodiscard]] net::RestRequest
    order_book(std::string_view settle, std::string_view contract, std::uint32_t depth) noexcept;

private:
    std::array<char, 256> target_{};
};

} // namespace raikiri::venues::gate
