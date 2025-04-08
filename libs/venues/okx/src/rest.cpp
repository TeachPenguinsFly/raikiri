#include <raikiri/venues/okx/rest.hpp>

#include <charconv>
#include <cstring>

namespace raikiri::venues::okx {

namespace {

constexpr std::string_view k_host = "www.okx.com";

// Fills the builder's target buffer. A part that would overflow is dropped
// rather than truncated mid-token, so an over-long instId yields a request
// OKX refuses with 51001 instead of one that names a different instrument.
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
    return {.method = net::HttpMethod::Get, .target = "/api/v5/public/time", .body = {}};
}

// Every builder below writes into the same buffer, so a request must be sent
// before the next one is built. One catalogue load at a time is the only use.
net::RestRequest RequestBuilder::instruments(std::string_view inst_type) noexcept {
    TargetWriter writer{target_};
    writer.put("/api/v5/public/instruments?instType=");
    writer.put(inst_type);
    return writer.get();
}

net::RestRequest RequestBuilder::mark_price(std::string_view inst_id) noexcept {
    TargetWriter writer{target_};
    writer.put("/api/v5/public/mark-price?instType=SWAP&instId=");
    writer.put(inst_id);
    return writer.get();
}

net::RestRequest RequestBuilder::funding_rate(std::string_view inst_id) noexcept {
    TargetWriter writer{target_};
    writer.put("/api/v5/public/funding-rate?instId=");
    writer.put(inst_id);
    return writer.get();
}

net::RestRequest RequestBuilder::books(std::string_view inst_id, std::uint32_t depth) noexcept {
    TargetWriter writer{target_};
    writer.put("/api/v5/market/books?instId=");
    writer.put(inst_id);
    writer.put("&sz=");
    writer.put(depth);
    return writer.get();
}

} // namespace raikiri::venues::okx
