#pragma once

#include <raikiri/net/rest_client.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace raikiri::venues::backpack {

class RequestBuilder {
public:
    [[nodiscard]] net::RestRequest markets() const noexcept;
    [[nodiscard]] net::RestRequest tickers() const noexcept;

    // the target views this builder's buffer until the next call; empty when the symbol does not fit
    [[nodiscard]] net::RestRequest depth_snapshot(std::string_view symbol, std::uint32_t limit) noexcept;

private:
    std::array<char, 128> target_{};
};

} // namespace raikiri::venues::backpack
