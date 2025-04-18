#include <raikiri/venues/bingx/rest.hpp>

#include <charconv>
#include <cstring>
#include <span>

namespace raikiri::venues::bingx {

namespace {

constexpr std::string_view k_host = "open-api.bingx.com";

// Fills the target buffer, dropping any part that would overflow rather than
// truncating mid-token: an over-long symbol must not name a different one.
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

net::RestRequest RequestBuilder::contracts() noexcept {
    return {
        .method = net::HttpMethod::Get, .target = "/openApi/swap/v2/quote/contracts", .body = {}};
}

net::RestRequest RequestBuilder::server_time() noexcept {
    return {.method = net::HttpMethod::Get, .target = "/openApi/swap/v2/server/time", .body = {}};
}

// Every builder below shares one buffer, so a request must be sent before the
// next is built. One catalogue load or funding poll at a time is the only use.
net::RestRequest RequestBuilder::premium_index(std::string_view symbol) noexcept {
    TargetWriter writer{target_};
    writer.put("/openApi/swap/v2/quote/premiumIndex?symbol=");
    writer.put(symbol);
    return writer.get();
}

net::RestRequest RequestBuilder::depth(std::string_view symbol, std::uint32_t limit) noexcept {
    TargetWriter writer{target_};
    writer.put("/openApi/swap/v2/quote/depth?symbol=");
    writer.put(symbol);
    writer.put("&limit=");
    writer.put(limit);
    return writer.get();
}

net::RestRequest RequestBuilder::ticker(std::string_view symbol) noexcept {
    TargetWriter writer{target_};
    writer.put("/openApi/swap/v2/quote/ticker?symbol=");
    writer.put(symbol);
    return writer.get();
}

} // namespace raikiri::venues::bingx
