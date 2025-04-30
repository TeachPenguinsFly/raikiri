#pragma once

// OKX v5 REST request building.
//
// There is no separate demo host: OKX switches an account to demo trading
// with an x-simulated-trading header on the same host. Public reads answer
// identically with or without it (verified), so nothing here takes a flag.

#include <raikiri/net/rest_client.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace raikiri::venues::okx {

[[nodiscard]] std::string_view rest_host() noexcept;

class RequestBuilder {
public:
    // /api/v5/public/instruments — one instType per call, roughly 450 KiB for
    // SWAP. Unlike binance's exchangeInfo there is no all-markets variant.
    [[nodiscard]] net::RestRequest instruments(std::string_view inst_type) noexcept;

    // /api/v5/public/time — used to measure the venue clock offset before
    // anything is signed.
    [[nodiscard]] net::RestRequest server_time() noexcept;

    [[nodiscard]] net::RestRequest mark_price(std::string_view inst_id) noexcept;

    [[nodiscard]] net::RestRequest funding_rate(std::string_view inst_id) noexcept;

    // /api/v5/market/books — the snapshot a depth subscription would start
    // from if anything consumed a book.
    [[nodiscard]] net::RestRequest books(std::string_view inst_id, std::uint32_t depth) noexcept;

private:
    std::array<char, 256> target_{};
};

} // namespace raikiri::venues::okx
