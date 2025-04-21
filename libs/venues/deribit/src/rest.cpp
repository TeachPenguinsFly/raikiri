#include <raikiri/venues/deribit/rest.hpp>

#include <cstring>

namespace raikiri::venues::deribit {

namespace {

constexpr std::string_view k_host_live = "www.deribit.com";
constexpr std::string_view k_host_test = "test.deribit.com";

// Drops a part that would overflow rather than truncating mid-token, so an
// over-long currency yields a request Deribit refuses with Invalid params
// instead of one that names a different market.
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

std::string_view rest_host(bool testnet) noexcept {
    return testnet ? k_host_test : k_host_live;
}

net::RestRequest RequestBuilder::server_time() noexcept {
    return {.method = net::HttpMethod::Get, .target = "/api/v2/public/get_time", .body = {}};
}

net::RestRequest RequestBuilder::instruments(std::string_view currency,
                                             std::string_view kind) noexcept {
    TargetWriter writer{target_};
    writer.put("/api/v2/public/get_instruments?currency=");
    writer.put(currency);
    writer.put("&kind=");
    writer.put(kind);
    return writer.get();
}

} // namespace raikiri::venues::deribit
