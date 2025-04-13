#include <raikiri/venues/coinbase/rest.hpp>

#include <charconv>
#include <cstring>
#include <span>

namespace raikiri::venues::coinbase {

namespace {

constexpr std::string_view k_host = "api.exchange.coinbase.com";

// Fills the builder's target buffer. A part that would overflow is dropped
// rather than truncated mid-token, so an over-long product id yields a
// request Coinbase answers with 404 NotFound instead of one that names a
// different product.
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

net::RestRequest RequestBuilder::products() noexcept {
    return {.method = net::HttpMethod::Get, .target = "/products", .body = {}};
}

net::RestRequest RequestBuilder::server_time() noexcept {
    return {.method = net::HttpMethod::Get, .target = "/time", .body = {}};
}

// Every builder below writes into the same buffer, so a request must be sent
// before the next one is built. One catalogue load at a time is the only use.
net::RestRequest RequestBuilder::product(std::string_view product_id) noexcept {
    TargetWriter writer{target_};
    writer.put("/products/");
    writer.put(product_id);
    return writer.get();
}

net::RestRequest RequestBuilder::book(std::string_view product_id, std::uint32_t level) noexcept {
    TargetWriter writer{target_};
    writer.put("/products/");
    writer.put(product_id);
    writer.put("/book?level=");
    writer.put(level);
    return writer.get();
}

} // namespace raikiri::venues::coinbase
