#include <raikiri/venues/gate/rest.hpp>

#include <charconv>
#include <cstring>
#include <span>

namespace raikiri::venues::gate {

namespace {

constexpr std::string_view k_host_live = "api.gateio.ws";
constexpr std::string_view k_host_testnet = "api-testnet.gateapi.io";

constexpr std::string_view k_prefix = "/api/v4";

// Fills the builder's target buffer. A part that would overflow is dropped
// rather than truncated mid-token, so an over-long contract name yields a
// request Gate refuses with CONTRACT_NOT_FOUND instead of one that names a
// different contract.
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
    return testnet ? k_host_testnet : k_host_live;
}

// Every builder below writes into the same buffer, so a request must be sent
// before the next one is built. One catalogue load at a time is the only use.
net::RestRequest RequestBuilder::contracts(std::string_view settle) noexcept {
    TargetWriter writer{target_};
    writer.put(k_prefix);
    writer.put("/futures/");
    writer.put(settle);
    writer.put("/contracts");
    return writer.get();
}

net::RestRequest RequestBuilder::server_time() noexcept {
    return {.method = net::HttpMethod::Get, .target = "/api/v4/spot/time", .body = {}};
}

net::RestRequest RequestBuilder::order_book(std::string_view settle,
                                            std::string_view contract,
                                            std::uint32_t depth) noexcept {
    TargetWriter writer{target_};
    writer.put(k_prefix);
    writer.put("/futures/");
    writer.put(settle);
    writer.put("/order_book?contract=");
    writer.put(contract);
    writer.put("&limit=");
    writer.put(depth);
    return writer.get();
}

} // namespace raikiri::venues::gate
