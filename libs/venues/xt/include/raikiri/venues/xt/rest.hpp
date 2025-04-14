#pragma once

// XT futures REST request building.
//
// Only the unauthenticated reads the market-data path needs. Signing arrives
// with order entry; XT carries the key and digest in `validate-*` headers
// (validate-appkey, validate-timestamp, validate-recvwindow,
// validate-signature, validate-algorithms) rather than in the query string,
// and an unsigned private call answers error.code "400" with "Absence of
// [appKey] in client request is prohibited". Recording that here keeps the
// next commit from rediscovering it.
//
// Two traps live in this file's URLs.
//
// Symbols are case-INSENSITIVE on the way in and ECHOED BACK verbatim: asking
// for BTC_USDT answers with "s":"BTC_USDT", which would then miss the
// lowercase symbol interned from the catalogue and drop every frame. Only the
// catalogue's own spelling is ever sent.
//
// There is no futures testnet host, so nothing here takes a flag.

#include <raikiri/net/rest_client.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace raikiri::venues::xt {

[[nodiscard]] std::string_view rest_host() noexcept;

class RequestBuilder {
public:
    // /future/market/v1/public/symbol/list — roughly 2.2 MiB and 1067 rows,
    // covering perpetuals and dated futures together; there is no per-market
    // variant, so the mapper does the splitting.
    [[nodiscard]] net::RestRequest symbol_list() noexcept;

    // /future/market/v1/public/time — used to measure the venue clock offset
    // before anything is signed.
    [[nodiscard]] net::RestRequest server_time() noexcept;

    // /future/market/v1/public/q/funding-rate — the ONLY source of funding on
    // this venue; XT streams no funding channel.
    [[nodiscard]] net::RestRequest funding_rate(std::string_view symbol) noexcept;

    // /future/market/v1/public/q/depth — the snapshot a depth_update
    // subscription would start from if anything consumed a book.
    [[nodiscard]] net::RestRequest depth(std::string_view symbol, std::uint32_t level) noexcept;

private:
    std::array<char, 256> target_{};
};

} // namespace raikiri::venues::xt
