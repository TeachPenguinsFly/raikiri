#pragma once

// Bitfinex v2 REST request building.

#include <raikiri/net/rest_client.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace raikiri::venues::bitfinex {

[[nodiscard]] std::string_view rest_host() noexcept;

class RequestBuilder {
public:
    [[nodiscard]] net::RestRequest pair_info(bool futures) noexcept;

    // /v2/platform/status — `[1]` operative, `[0]` maintenance.
    [[nodiscard]] net::RestRequest platform_status() noexcept;

    [[nodiscard]] net::RestRequest tickers() noexcept;

    [[nodiscard]] net::RestRequest book(std::string_view symbol, std::uint32_t len) noexcept;

private:
    std::array<char, 256> target_{};
};

} // namespace raikiri::venues::bitfinex
