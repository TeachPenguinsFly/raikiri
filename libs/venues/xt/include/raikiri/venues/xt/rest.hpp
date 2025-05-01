#pragma once

// XT futures REST request building.

#include <raikiri/net/rest_client.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace raikiri::venues::xt {

[[nodiscard]] std::string_view rest_host() noexcept;

class RequestBuilder {
public:
    [[nodiscard]] net::RestRequest symbol_list() noexcept;

    [[nodiscard]] net::RestRequest server_time() noexcept;

    [[nodiscard]] net::RestRequest funding_rate(std::string_view symbol) noexcept;

    [[nodiscard]] net::RestRequest depth(std::string_view symbol, std::uint32_t level) noexcept;

private:
    std::array<char, 256> target_{};
};

} // namespace raikiri::venues::xt
