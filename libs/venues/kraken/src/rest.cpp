#include <raikiri/venues/kraken/rest.hpp>

#include <cstring>
#include <span>

namespace raikiri::venues::kraken {

namespace {

constexpr std::string_view k_host = "futures.kraken.com";
constexpr std::string_view k_base = "/derivatives/api/v3";

// Fills the builder's target buffer. An overflowing part is dropped rather
// than truncated mid-token, so an over-long symbol yields a request Kraken
// refuses instead of one naming a different instrument.
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

// Every builder writes the same buffer, so a request must be sent before the
// next is built. One catalogue load at a time is the only use.
net::RestRequest RequestBuilder::instruments() noexcept {
    TargetWriter writer{target_};
    writer.put(k_base);
    writer.put("/instruments");
    return writer.get();
}

net::RestRequest RequestBuilder::ticker(std::string_view symbol) noexcept {
    TargetWriter writer{target_};
    writer.put(k_base);
    writer.put("/tickers/");
    writer.put(symbol);
    return writer.get();
}

net::RestRequest RequestBuilder::orderbook(std::string_view symbol) noexcept {
    TargetWriter writer{target_};
    writer.put(k_base);
    writer.put("/orderbook?symbol=");
    writer.put(symbol);
    return writer.get();
}

} // namespace raikiri::venues::kraken
