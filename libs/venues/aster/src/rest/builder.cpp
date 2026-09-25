#include <raikiri/venues/aster/endpoints.hpp>
#include <raikiri/venues/aster/rest.hpp>

#include <charconv>
#include <cstring>

namespace raikiri::venues::aster {

namespace {

constexpr std::string_view k_hdr_key = "X-MBX-APIKEY";
constexpr std::string_view k_form = "application/x-www-form-urlencoded";

// tracks overflow; a truncated query would still be signed
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

    [[nodiscard]] bool ok() const noexcept { return !overflow_; }

    [[nodiscard]] std::size_t size() const noexcept { return len_; }

    [[nodiscard]] std::string_view view() const noexcept { return {buf_.data(), len_}; }

private:
    std::span<char> buf_;
    std::size_t len_{0};
    bool overflow_{false};
};

} // namespace

std::string_view to_string(BuildError err) noexcept {
    switch (err) {
    case BuildError::NoCredentials:
        return "NoCredentials";
    case BuildError::BufferTooSmall:
        return "BufferTooSmall";
    case BuildError::UnsupportedOrderType:
        return "UnsupportedOrderType";
    case BuildError::BadValue:
        return "BadValue";
    }
    return "Unknown";
}

std::string_view rest_host(bool testnet) noexcept {
    return ep::rest_host(testnet);
}

net::RestRequest RequestBuilder::instruments() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_exchange_info, .body = {}};
}

// one call returns every symbol
net::RestRequest RequestBuilder::ticker_24h() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_ticker_24h, .body = {}};
}

net::RestRequest RequestBuilder::premium_index() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_premium_index, .body = {}};
}

net::RestRequest RequestBuilder::funding_info() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_funding_info, .body = {}};
}

net::RestRequest RequestBuilder::server_time() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_server_time, .body = {}};
}

net::RestRequest RequestBuilder::depth_snapshot(std::string_view symbol, std::uint32_t limit) noexcept {
    Appender target{target_};
    target << ep::k_depth << "?symbol=" << symbol << "&limit=" << std::uint64_t{limit};
    return {.method = net::HttpMethod::Get, .target = target.ok() ? target.view() : std::string_view{}, .body = {}};
}

std::string_view RequestBuilder::last_sign_message() const noexcept {
    return {query_.data(), sign_len_};
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::finish(net::HttpMethod method, std::string_view path, std::size_t query_len) {
    if (query_len == 0 || query_len > query_.size()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    sign_len_ = query_len;
    const std::string_view query{query_.data(), query_len};
    const auto signature = signer_->sign(query, net::Signer::Encoding::Hex, signature_);
    if (signature.empty()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }

    Appender target{target_};
    target << path << "?" << query << "&signature=" << signature;
    if (!target.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    net::RestRequest request{};
    request.method = method;
    request.target = target.view();
    request.body = {};
    request.content_type = k_form;
    request.headers = {{k_hdr_key, signer_->api_key()}};
    return request;
}

net::RestRequest RequestBuilder::header_only(net::HttpMethod method, std::string_view path) noexcept {
    net::RestRequest request{};
    request.method = method;
    request.target = path;
    request.body = {};
    request.content_type = k_form;
    request.headers = {{k_hdr_key, signer_->api_key()}};
    return request;
}

} // namespace raikiri::venues::aster
