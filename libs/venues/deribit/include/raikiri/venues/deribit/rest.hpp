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
    // /public/get_instruments — one currency and one kind per call. `kind` is
    // "future" or "option"; there is no "perpetual" kind, so the perpetuals
    // arrive inside "future" and the mapper separates them.
    [[nodiscard]] net::RestRequest instruments(std::string_view currency,
                                               std::string_view kind) noexcept;

    // Measures the venue clock offset. Nothing is signed here, but a wrong
    // offset shows up as ts_event skew in a basis series.
    [[nodiscard]] net::RestRequest server_time() noexcept;

private:
    // Every builder writes into this buffer, so a request must be sent before
    // the next one is built. One catalogue load at a time is the only use.
    std::array<char, 256> target_{};
};

} // namespace raikiri::venues::deribit
