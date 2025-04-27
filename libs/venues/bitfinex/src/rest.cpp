#include <raikiri/venues/bitfinex/rest.hpp>

#include <charconv>
#include <cstring>
#include <span>

namespace raikiri::venues::bitfinex {

namespace {

// The public host. Bitfinex splits reads from trading (api.bitfinex.com), and
// only the read side is used here.
constexpr std::string_view k_host = "api-pub.bitfinex.com";

// Fills the builder's target buffer. A part that would overflow is dropped
// rather than truncated mid-token, so an over-long symbol yields a request
// Bitfinex refuses with 10020 instead of one that names a different pair.
class TargetWriter {
public:
    explicit TargetWriter(std::span<char> into) noexcept : into_{into} {}

    void put(std::string_view text) noexcept {
        if (len_ + text.size() > into_.size()) {
            return;
        }
        std::memcpy(into_.data() + len_, text.data(), text.size());
        len_ += text.size();
    }

    void put(std::uint32_t value) noexcept {
        std::array<char, 16> num{};
        if (const auto [stop, ec] = std::to_chars(num.data(), num.data() + num.size(), value);
            ec == std::errc{}) {
            put(std::string_view{num.data(), static_cast<std::size_t>(stop - num.data())});
        }
    }

    [[nodiscard]] net::RestRequest get() const noexcept {
        return {.method = net::HttpMethod::Get,
                .target = std::string_view{into_.data(), len_},
                .body = {}};
    }

private:
    std::span<char> into_;
    std::size_t len_{0};
};

} // namespace

std::string_view rest_host() noexcept {
    return k_host;
}

net::RestRequest RequestBuilder::platform_status() noexcept {
    return {.method = net::HttpMethod::Get, .target = "/v2/platform/status", .body = {}};
}

net::RestRequest RequestBuilder::tickers() noexcept {
    return {.method = net::HttpMethod::Get, .target = "/v2/tickers?symbols=ALL", .body = {}};
}

// Every builder below writes into the same buffer, so a request must be sent
// before the next one is built. One catalogue load at a time is the only use.
net::RestRequest RequestBuilder::pair_info(bool futures) noexcept {
    TargetWriter writer{target_};
    writer.put("/v2/conf/pub:info:pair");
    if (futures) {
        writer.put(":futures");
    }
    return writer.get();
}

net::RestRequest RequestBuilder::book(std::string_view symbol, std::uint32_t len) noexcept {
    TargetWriter writer{target_};
    writer.put("/v2/book/");
    writer.put(symbol);
    writer.put("/P0?len=");
    writer.put(len);
    return writer.get();
}

} // namespace raikiri::venues::bitfinex
