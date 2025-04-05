#pragma once

// Binance USD-M futures REST request building.
//
// Only the unauthenticated reads the market-data path needs: the catalogue,
// server time and a depth snapshot. Signing arrives with order entry, and
// its shape is already decided — binance signs the QUERY STRING for every
// method, with the parameters in the exact order they appear on the wire,
// and carries the key in an X-MBX-APIKEY header rather than in the payload.
// Recording that here keeps the next commit from rediscovering it.

#include <raikiri/net/rest_client.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace raikiri::venues::binance {

[[nodiscard]] std::string_view rest_host(bool testnet) noexcept;

class RequestBuilder {
public:
    // /fapi/v1/exchangeInfo — the whole catalogue, roughly 1 MiB.
    [[nodiscard]] net::RestRequest instruments() noexcept;

    // /fapi/v1/time — used to measure the venue clock offset before anything
    // is signed.
    [[nodiscard]] net::RestRequest server_time() noexcept;

    // /fapi/v1/depth — the snapshot a depth subscription has to start from.
    [[nodiscard]] net::RestRequest depth_snapshot(std::string_view symbol,
                                                  std::uint32_t limit) noexcept;

private:
    std::array<char, 256> target_{};
};

} // namespace raikiri::venues::binance
