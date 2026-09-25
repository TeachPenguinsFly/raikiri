#include <raikiri/venues/backpack/endpoints.hpp>
#include <raikiri/venues/backpack/rest.hpp>

#include <charconv>
#include <cstring>

namespace raikiri::venues::backpack {

namespace {

net::RestRequest get(std::string_view target) noexcept {
    return {.method = net::HttpMethod::Get, .target = target, .body = {}};
}

} // namespace

net::RestRequest RequestBuilder::markets() const noexcept {
    return get(ep::k_markets);
}

net::RestRequest RequestBuilder::tickers() const noexcept {
    return get(ep::k_tickers);
}

net::RestRequest RequestBuilder::depth_snapshot(std::string_view symbol, std::uint32_t limit) noexcept {
    std::size_t len = 0;
    bool overflow = false;
    const auto put = [&](std::string_view text) {
        if (overflow || len + text.size() > target_.size()) {
            overflow = true;
            return;
        }
        std::memcpy(target_.data() + len, text.data(), text.size());
        len += text.size();
    };
    put(ep::k_depth);
    put("?symbol=");
    put(symbol);
    put("&limit=");
    std::array<char, 16> num{};
    const auto [stop, ec] = std::to_chars(num.data(), num.data() + num.size(), limit);
    if (ec != std::errc{}) {
        overflow = true;
    }
    put({num.data(), static_cast<std::size_t>(stop - num.data())});
    return get(overflow ? std::string_view{} : std::string_view{target_.data(), len});
}

} // namespace raikiri::venues::backpack
