#include <raikiri/venues/bitmex/rest.hpp>

#include <charconv>
#include <cstring>
#include <span>

namespace raikiri::venues::bitmex {

namespace {

constexpr std::string_view k_host_live = "www.bitmex.com";
constexpr std::string_view k_host_test = "testnet.bitmex.com";

// Fills the builder's target buffer. A part that would overflow is dropped
// rather than truncated mid-token, so an over-long symbol yields a request
// BitMEX refuses with a 400 instead of one that names a different instrument.
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

std::string_view rest_host(bool testnet) noexcept {
    return testnet ? k_host_test : k_host_live;
}

// Every builder below writes into the same buffer, so a request must be sent
// before the next one is built. One catalogue load at a time is the only use.
net::RestRequest RequestBuilder::instruments() noexcept {
    TargetWriter writer{target_};
    writer.put("/api/v1/instrument/active");
    return writer.get();
}

net::RestRequest RequestBuilder::instrument(std::string_view symbol) noexcept {
    TargetWriter writer{target_};
    writer.put("/api/v1/instrument?symbol=");
    writer.put(symbol);
    return writer.get();
}

net::RestRequest RequestBuilder::book(std::string_view symbol, std::uint32_t depth) noexcept {
    TargetWriter writer{target_};
    writer.put("/api/v1/orderBook/L2?symbol=");
    writer.put(symbol);
    writer.put("&depth=");
    writer.put(depth);
    return writer.get();
}

net::RestRequest RequestBuilder::trades(std::string_view symbol, std::uint32_t count) noexcept {
    TargetWriter writer{target_};
    writer.put("/api/v1/trade?symbol=");
    writer.put(symbol);
    writer.put("&reverse=true&count=");
    writer.put(count);
    return writer.get();
}

} // namespace raikiri::venues::bitmex
