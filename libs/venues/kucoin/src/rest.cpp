#include <raikiri/venues/kucoin/rest.hpp>

#include <cstring>
#include <span>

namespace raikiri::venues::kucoin {

namespace {

constexpr std::string_view k_host = "api-futures.kucoin.com";

// Fills the builder's target buffer. A part that would overflow is dropped
// rather than truncated mid-token, so an over-long symbol yields a request
// KuCoin refuses with 404000 instead of one that names a different contract.
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

net::RestRequest RequestBuilder::bullet_public() noexcept {
    return {.method = net::HttpMethod::Post, .target = "/api/v1/bullet-public", .body = {}};
}

net::RestRequest RequestBuilder::contracts_active() noexcept {
    return {.method = net::HttpMethod::Get, .target = "/api/v1/contracts/active", .body = {}};
}

net::RestRequest RequestBuilder::server_time() noexcept {
    return {.method = net::HttpMethod::Get, .target = "/api/v1/timestamp", .body = {}};
}

// Every builder below writes into the same buffer, so a request must be sent
// before the next one is built. One cold read at a time is the only use.
net::RestRequest RequestBuilder::funding_rate_current(std::string_view symbol) noexcept {
    TargetWriter writer{target_};
    writer.put("/api/v1/funding-rate/");
    writer.put(symbol);
    writer.put("/current");
    return writer.get();
}

net::RestRequest RequestBuilder::level2_snapshot(std::string_view symbol) noexcept {
    TargetWriter writer{target_};
    writer.put("/api/v1/level2/snapshot?symbol=");
    writer.put(symbol);
    return writer.get();
}

} // namespace raikiri::venues::kucoin
