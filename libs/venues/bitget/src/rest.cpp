#include <raikiri/venues/bitget/rest.hpp>

#include <charconv>
#include <cstring>
#include <span>

namespace raikiri::venues::bitget {

namespace {

constexpr std::string_view k_host = "api.bitget.com";
constexpr std::string_view k_product_live = "USDT-FUTURES";
constexpr std::string_view k_product_demo = "SUSDT-FUTURES";

// Fills the builder's target buffer. A part that would overflow is dropped
// rather than truncated mid-token, so an over-long symbol yields a request
// Bitget refuses with 40034 instead of one that names a different contract.
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

std::string_view product_type(bool testnet) noexcept {
    return testnet ? k_product_demo : k_product_live;
}

net::RestRequest RequestBuilder::server_time() noexcept {
    return {.method = net::HttpMethod::Get, .target = "/api/v2/public/time", .body = {}};
}

// Every builder below writes into the same buffer, so a request must be sent
// before the next one is built. One catalogue load at a time is the only use.
net::RestRequest RequestBuilder::contracts(std::string_view product) noexcept {
    TargetWriter writer{target_};
    writer.put("/api/v2/mix/market/contracts?productType=");
    writer.put(product);
    return writer.get();
}

net::RestRequest RequestBuilder::ticker(std::string_view symbol,
                                        std::string_view product) noexcept {
    TargetWriter writer{target_};
    writer.put("/api/v2/mix/market/ticker?symbol=");
    writer.put(symbol);
    writer.put("&productType=");
    writer.put(product);
    return writer.get();
}

net::RestRequest RequestBuilder::current_fund_rate(std::string_view symbol,
                                                   std::string_view product) noexcept {
    TargetWriter writer{target_};
    writer.put("/api/v2/mix/market/current-fund-rate?symbol=");
    writer.put(symbol);
    writer.put("&productType=");
    writer.put(product);
    return writer.get();
}

net::RestRequest RequestBuilder::orderbook(std::string_view symbol,
                                           std::string_view product,
                                           std::uint32_t depth) noexcept {
    TargetWriter writer{target_};
    writer.put("/api/v2/mix/market/orderbook?symbol=");
    writer.put(symbol);
    writer.put("&productType=");
    writer.put(product);
    writer.put("&limit=");
    writer.put(depth);
    return writer.get();
}

} // namespace raikiri::venues::bitget
