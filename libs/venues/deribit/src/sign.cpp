#include <raikiri/venues/deribit/sign.hpp>

#include <charconv>
#include <cstring>

namespace raikiri::venues::deribit::sign {

namespace {

class Appender {
public:
    explicit Appender(std::span<char> into) noexcept : into_{into} {}

    Appender& operator<<(std::string_view text) noexcept {
        if (overflow_ || len_ + text.size() > into_.size()) {
            overflow_ = true;
            return *this;
        }
        std::memcpy(into_.data() + len_, text.data(), text.size());
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
        return overflow_ ? std::string_view{} : std::string_view{into_.data(), len_};
    }

private:
    std::span<char> into_;
    std::size_t len_{0};
    bool overflow_{false};
};

[[nodiscard]] std::string_view method_text(net::HttpMethod method) noexcept {
    switch (method) {
    case net::HttpMethod::Get:
        return "GET";
    case net::HttpMethod::Post:
        return "POST";
    case net::HttpMethod::Put:
        return "PUT";
    case net::HttpMethod::Delete:
        return "DELETE";
    case net::HttpMethod::Patch:
        return "PATCH";
    }
    return "GET";
}

} // namespace

std::string_view request_data(net::HttpMethod method,
                              std::string_view uri,
                              std::string_view body,
                              std::span<char> out) noexcept {
    Appender text{out};
    // deribit expects the trailing newline even for an empty body
    text << method_text(method) << "\n" << uri << "\n" << body << "\n";
    return text.view();
}

std::string_view message(std::uint64_t timestamp_ms,
                         std::string_view nonce,
                         std::string_view data,
                         std::span<char> out) noexcept {
    Appender text{out};
    text << timestamp_ms << "\n" << nonce << "\n" << data;
    return text.view();
}

std::string_view nonce(std::uint64_t counter, std::span<char> out) noexcept {
    std::array<char, 20> hex{};
    const auto [stop, ec] = std::to_chars(hex.data(), hex.data() + hex.size(), counter, 16);
    if (ec != std::errc{}) {
        return {};
    }
    Appender text{out};
    text << "n" << std::string_view{hex.data(), static_cast<std::size_t>(stop - hex.data())};
    return text.view();
}

std::string_view auth_header(const net::Signer& signer,
                             std::uint64_t timestamp_ms,
                             std::string_view nonce_text,
                             std::string_view signature,
                             std::span<char> out) noexcept {
    if (!signer.has_credentials() || signature.empty() || nonce_text.empty()) {
        return {};
    }
    Appender text{out};
    text << "deri-hmac-sha256 id=" << signer.api_key() << ",ts=" << timestamp_ms
         << ",nonce=" << nonce_text << ",sig=" << signature;
    return text.view();
}

} // namespace raikiri::venues::deribit::sign
