#include <raikiri/venues/coinbase/sign.hpp>

#include <array>
#include <charconv>
#include <cstring>

namespace raikiri::venues::coinbase {

namespace {

constexpr std::string_view k_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

[[nodiscard]] int sextet(char c) noexcept {
    const auto at = k_alphabet.find(c);
    return at == std::string_view::npos ? -1 : static_cast<int>(at);
}

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

class Appender {
public:
    explicit Appender(std::span<char> buf) noexcept : buf_{buf} {}

    Appender& operator<<(std::string_view text) noexcept {
        if (overflow_ || len_ + text.size() > buf_.size()) {
            overflow_ = true;
            return *this;
        }
        text.copy(buf_.data() + len_, text.size());
        len_ += text.size();
        return *this;
    }

    [[nodiscard]] std::string_view view() const noexcept {
        return overflow_ ? std::string_view{} : std::string_view{buf_.data(), len_};
    }

private:
    std::span<char> buf_;
    std::size_t len_{0};
    bool overflow_{false};
};

} // namespace

std::optional<std::size_t> decode_secret(std::string_view base64_secret, std::span<char> out) noexcept {
    if (base64_secret.empty() || base64_secret.size() % 4 != 0) {
        return std::nullopt;
    }
    std::size_t written = 0;
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (const char c : base64_secret) {
        if (c == '=') {
            break; // padding: whatever is buffered is not a whole byte
        }
        const auto value = sextet(c);
        if (value < 0) {
            return std::nullopt;
        }
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits < 8) {
            continue;
        }
        bits -= 8;
        if (written == out.size()) {
            return std::nullopt;
        }
        out[written++] = static_cast<char>((accumulator >> bits) & 0xFFU);
    }
    return written == 0 ? std::nullopt : std::optional{written};
}

std::string_view prehash(std::string_view timestamp,
                         net::HttpMethod method,
                         std::string_view request_path,
                         std::string_view body,
                         std::span<char> out) noexcept {
    Appender text{out};
    // no separators between the four parts
    text << timestamp << method_text(method) << request_path << body;
    return text.view();
}

std::string_view timestamp_s(UnixNanos now, std::span<char> out) noexcept {
    std::array<char, 24> num{};
    const auto seconds = now.ns / 1'000'000'000ULL;
    const auto [stop, ec] = std::to_chars(num.data(), num.data() + num.size(), seconds);
    if (ec != std::errc{}) {
        return {};
    }
    Appender text{out};
    text << std::string_view{num.data(), static_cast<std::size_t>(stop - num.data())};
    return text.view();
}

} // namespace raikiri::venues::coinbase
