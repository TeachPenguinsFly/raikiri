#pragma once

// MEXC futures REST request building.

#include <raikiri/net/rest_client.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace raikiri::venues::mexc {

[[nodiscard]] std::string_view rest_host() noexcept;

class RequestBuilder {
public:
    // /api/v1/contract/detail — the whole futures catalogue in one call,
    // roughly 2 MiB for 1086 rows. There is no per-symbol or per-market
    // variant and no query parameter to narrow it.
    [[nodiscard]] net::RestRequest contract_detail() noexcept;

    // /api/v1/contract/ping — the venue clock, despite the name; its `data` is
    // the epoch-millisecond number.
    [[nodiscard]] net::RestRequest server_time() noexcept;

    [[nodiscard]] net::RestRequest depth(std::string_view symbol, std::uint32_t limit) noexcept;

    [[nodiscard]] net::RestRequest ticker(std::string_view symbol) noexcept;

    // The only place the funding SCHEDULE exists: the websocket ticker carries
    // a rate but never a settlement time.
    [[nodiscard]] net::RestRequest funding_rate(std::string_view symbol) noexcept;

private:
    std::array<char, 256> target_{};
};

} // namespace raikiri::venues::mexc
