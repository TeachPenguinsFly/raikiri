#include <raikiri/venues/binance/rest.hpp>

#include <charconv>
#include <cstring>

namespace raikiri::venues::binance {

namespace {

constexpr std::string_view k_host_live = "fapi.binance.com";
constexpr std::string_view k_host_test = "testnet.binancefuture.com";

} // namespace

std::string_view rest_host(bool testnet) noexcept {
    return testnet ? k_host_test : k_host_live;
}

net::RestRequest RequestBuilder::instruments() noexcept {
    return {.method = net::HttpMethod::Get, .target = "/fapi/v1/exchangeInfo", .body = {}};
}

net::RestRequest RequestBuilder::server_time() noexcept {
    return {.method = net::HttpMethod::Get, .target = "/fapi/v1/time", .body = {}};
}

net::RestRequest RequestBuilder::depth_snapshot(std::string_view symbol,
                                                std::uint32_t limit) noexcept {
    std::size_t len = 0;
    const auto put = [&](std::string_view text) {
        if (len + text.size() > target_.size()) {
            return;
        }
        std::memcpy(target_.data() + len, text.data(), text.size());
        len += text.size();
    };
    put("/fapi/v1/depth?symbol=");
    put(symbol);
    put("&limit=");
    std::array<char, 16> num{};
    if (const auto [stop, ec] = std::to_chars(num.data(), num.data() + num.size(), limit);
        ec == std::errc{}) {
        put({num.data(), static_cast<std::size_t>(stop - num.data())});
    }
    return {.method = net::HttpMethod::Get,
            .target = std::string_view{target_.data(), len},
            .body = {}};
}

} // namespace raikiri::venues::binance
