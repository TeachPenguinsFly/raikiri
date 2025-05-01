#pragma once

// Deribit v2 REST request building.
//
// Only the unauthenticated reads the market-data path needs; order entry is
// permanently out of scope here, so nothing anticipates signing.
//
// Everything is JSON-RPC 2.0 even over REST: the METHOD is the path and the
// params are the query string, while the body is a JSON-RPC envelope rather
// than the bare payload — which is why one reply parser serves both
// transports.

#include <raikiri/net/rest_client.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace raikiri::venues::deribit {

[[nodiscard]] std::string_view rest_host(bool testnet) noexcept;

// The coin-margined complex, which is the whole reason this venue is here:
// USDC-settled series are linear and the mapper refuses them, so pulling them
// would be bytes spent to be discarded.
inline constexpr std::array<std::string_view, 2> k_currencies{"BTC", "ETH"};

class RequestBuilder {
public:
    [[nodiscard]] net::RestRequest instruments(std::string_view currency,
                                               std::string_view kind) noexcept;

    // Measures the venue clock offset.
    [[nodiscard]] net::RestRequest server_time() noexcept;

private:
    std::array<char, 256> target_{};
};

} // namespace raikiri::venues::deribit
