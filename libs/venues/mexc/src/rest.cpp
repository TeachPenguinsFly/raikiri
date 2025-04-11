#include <raikiri/venues/mexc/rest.hpp>

#include <charconv>
#include <cstring>
#include <span>

namespace raikiri::venues::mexc {

namespace {

constexpr std::string_view k_host = "contract.mexc.com";

// Fills the builder's target buffer. A part that would overflow is dropped
// rather than truncated mid-token, so an over-long symbol yields a request
// MEXC refuses with 1001 instead of one that names a different contract.
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

net::RestRequest RequestBuilder::server_time() noexcept {
    return {.method = net::HttpMethod::Get, .target = "/api/v1/contract/ping", .body = {}};
}

net::RestRequest RequestBuilder::contract_detail() noexcept {
    return {.method = net::HttpMethod::Get, .target = "/api/v1/contract/detail", .body = {}};
}

// Every builder below writes into the same buffer, so a request must be sent
// before the next one is built. One catalogue load at a time is the only use.
net::RestRequest RequestBuilder::depth(std::string_view symbol, std::uint32_t limit) noexcept {
    TargetWriter writer{target_};
    writer.put("/api/v1/contract/depth/");
    writer.put(symbol);
    if (limit != 0) {
        writer.put("?limit=");
        writer.put(limit);
    }
    return writer.get();
}

net::RestRequest RequestBuilder::ticker(std::string_view symbol) noexcept {
    TargetWriter writer{target_};
    writer.put("/api/v1/contract/ticker?symbol=");
    writer.put(symbol);
    return writer.get();
}

net::RestRequest RequestBuilder::funding_rate(std::string_view symbol) noexcept {
    TargetWriter writer{target_};
    writer.put("/api/v1/contract/funding_rate/");
    writer.put(symbol);
    return writer.get();
}

} // namespace raikiri::venues::mexc
