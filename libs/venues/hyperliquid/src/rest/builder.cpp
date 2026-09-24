#include <raikiri/venues/hyperliquid/rest.hpp>

#include <charconv>
#include <cstring>
#include <span>
#include <string>

namespace raikiri::venues::hyperliquid {

namespace {

constexpr std::string_view k_host_live = "api.hyperliquid.xyz";
constexpr std::string_view k_host_testnet = "api.hyperliquid-testnet.xyz";
constexpr std::string_view k_info = "/info";
constexpr std::string_view k_exchange = "/exchange";

// reports overflow instead of truncating; a truncated body is a different query
class Appender {
public:
    explicit Appender(std::span<char> buf) noexcept : buf_{buf} {}

    Appender& operator<<(std::string_view text) noexcept {
        if (overflow_ || len_ + text.size() > buf_.size()) {
            overflow_ = true;
            return *this;
        }
        std::memcpy(buf_.data() + len_, text.data(), text.size());
        len_ += text.size();
        return *this;
    }

    Appender& operator<<(std::uint64_t value) noexcept {
        std::array<char, 24> num{};
        const auto [stop, ec] = std::to_chars(num.data(), num.data() + num.size(), value);
        if (ec != std::errc{}) {
            overflow_ = true;
            return *this;
        }
        return *this << std::string_view{num.data(), static_cast<std::size_t>(stop - num.data())};
    }

    [[nodiscard]] std::string_view view() const noexcept {
        return overflow_ ? std::string_view{} : std::string_view{buf_.data(), len_};
    }

private:
    std::span<char> buf_;
    std::size_t len_{0};
    bool overflow_{false};
};

[[nodiscard]] net::RestRequest info(std::string_view body) noexcept {
    return {.method = net::HttpMethod::Post, .target = k_info, .body = body};
}

} // namespace

std::string_view rest_host(bool testnet) noexcept {
    return testnet ? k_host_testnet : k_host_live;
}

std::string_view info_path() noexcept {
    return k_info;
}

std::string_view exchange_path() noexcept {
    return k_exchange;
}

net::RestRequest RequestBuilder::meta() noexcept {
    return info(R"({"type":"meta"})");
}

net::RestRequest RequestBuilder::meta(std::string_view dex, std::string& body) {
    body.assign(R"({"type":"meta","dex":")");
    body.append(dex);
    body.append(R"("})");
    return info(body);
}

net::RestRequest RequestBuilder::perp_dexs() noexcept {
    return info(R"({"type":"perpDexs"})");
}

net::RestRequest RequestBuilder::perp_categories() noexcept {
    return info(R"({"type":"perpCategories"})");
}

net::RestRequest RequestBuilder::meta_and_asset_ctxs() noexcept {
    return info(R"({"type":"metaAndAssetCtxs"})");
}

net::RestRequest RequestBuilder::spot_meta() noexcept {
    return info(R"({"type":"spotMeta"})");
}

net::RestRequest UserRequestBuilder::simple(std::string_view type, std::string_view user) noexcept {
    Appender body{body_};
    body << R"({"type":")" << type << R"(","user":")" << user << R"("})";
    return info(body.view());
}

net::RestRequest UserRequestBuilder::clearinghouse_state(std::string_view user) noexcept {
    return simple("clearinghouseState", user);
}

net::RestRequest UserRequestBuilder::spot_state(std::string_view user) noexcept {
    return simple("spotClearinghouseState", user);
}

net::RestRequest UserRequestBuilder::open_orders(std::string_view user) noexcept {
    return simple("openOrders", user);
}

net::RestRequest UserRequestBuilder::frontend_open_orders(std::string_view user) noexcept {
    return simple("frontendOpenOrders", user);
}

net::RestRequest UserRequestBuilder::user_fills(std::string_view user) noexcept {
    return simple("userFills", user);
}

net::RestRequest UserRequestBuilder::user_fills_by_time(std::string_view user,
                                                        std::uint64_t start_ms) noexcept {
    Appender body{body_};
    body << R"({"type":"userFillsByTime","user":")" << user << R"(","startTime":)" << start_ms
         << "}";
    return info(body.view());
}

net::RestRequest UserRequestBuilder::user_funding(std::string_view user,
                                                  std::uint64_t start_ms) noexcept {
    Appender body{body_};
    body << R"({"type":"userFunding","user":")" << user << R"(","startTime":)" << start_ms << "}";
    return info(body.view());
}

net::RestRequest UserRequestBuilder::order_status(std::string_view user,
                                                  std::uint64_t oid) noexcept {
    Appender body{body_};
    body << R"({"type":"orderStatus","user":")" << user << R"(","oid":)" << oid << "}";
    return info(body.view());
}

net::RestRequest UserRequestBuilder::order_status(std::string_view user,
                                                  std::string_view cloid) noexcept {
    Appender body{body_};
    body << R"({"type":"orderStatus","user":")" << user << R"(","oid":")" << cloid << R"("})";
    return info(body.view());
}

} // namespace raikiri::venues::hyperliquid
