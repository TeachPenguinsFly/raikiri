#include <raikiri/venues/binance/ws_api.hpp>
#include <raikiri/wire/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstring>

namespace raikiri::venues::binance {

namespace {

struct Param {
    std::string_view key;
    std::string_view value;
};

constexpr std::size_t k_max_params = 16;

[[nodiscard]] int hex_digit(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

// the rest query percent-encodes; the json carries the decoded text
[[nodiscard]] std::string_view decode(std::string_view text, std::span<char> out, std::size_t& used) noexcept {
    const auto start = used;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (used == out.size()) {
            return {};
        }
        char c = text[i];
        if (c == '%' && i + 2 < text.size() && hex_digit(text[i + 1]) >= 0 && hex_digit(text[i + 2]) >= 0) {
            c = static_cast<char>((hex_digit(text[i + 1]) << 4) | hex_digit(text[i + 2]));
            i += 2;
        }
        out[used++] = c;
    }
    return {out.data() + start, used - start};
}

[[nodiscard]] bool numeric_key(std::string_view key) noexcept {
    return key == "timestamp" || key == "recvWindow" || key == "orderId";
}

} // namespace

std::expected<std::string_view, BuildError>
WsApiBuilder::request(std::uint64_t id, std::string_view method, std::string_view query) {
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NoCredentials};
    }
    std::array<Param, k_max_params> params{};
    std::size_t count = 0;
    std::size_t used = 0;
    params[count++] = {.key = "apiKey", .value = signer_->api_key()};
    while (!query.empty()) {
        const auto amp = query.find('&');
        const auto pair = query.substr(0, amp);
        query = amp == std::string_view::npos ? std::string_view{} : query.substr(amp + 1);
        const auto eq = pair.find('=');
        if (eq == std::string_view::npos || count == params.size()) {
            return std::unexpected{BuildError::BadValue};
        }
        const auto value = decode(pair.substr(eq + 1), values_, used);
        if (value.empty()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        // values are written unescaped
        if (value.find_first_of("\"\\") != std::string_view::npos) {
            return std::unexpected{BuildError::BadValue};
        }
        params[count++] = {.key = pair.substr(0, eq), .value = value};
    }
    std::sort(params.begin(), params.begin() + static_cast<std::ptrdiff_t>(count), [](const Param& a, const Param& b) {
        return a.key < b.key;
    });

    std::size_t len = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto need = params[i].key.size() + params[i].value.size() + (i == 0 ? 1 : 2);
        if (len + need > sign_.size()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        if (i != 0) {
            sign_[len++] = '&';
        }
        std::memcpy(sign_.data() + len, params[i].key.data(), params[i].key.size());
        len += params[i].key.size();
        sign_[len++] = '=';
        std::memcpy(sign_.data() + len, params[i].value.data(), params[i].value.size());
        len += params[i].value.size();
    }
    sign_len_ = len;
    const auto signature = signer_->sign(last_sign_message(), net::Signer::Encoding::Hex, signature_);
    if (signature.empty()) {
        return std::unexpected{BuildError::NoCredentials};
    }

    std::size_t at = 0;
    const auto put = [&](std::string_view text) {
        if (at + text.size() > frame_.size()) {
            at = frame_.size() + 1;
            return;
        }
        text.copy(frame_.data() + at, text.size());
        at += text.size();
    };
    std::array<char, 24> digits{};
    const auto [stop, ec] = std::to_chars(digits.data(), digits.data() + digits.size(), id);
    if (ec != std::errc{}) {
        return std::unexpected{BuildError::BadValue};
    }
    put(R"({"id":")");
    put({digits.data(), static_cast<std::size_t>(stop - digits.data())});
    put(R"(","method":")");
    put(method);
    put(R"(","params":{)");
    for (std::size_t i = 0; i < count && at <= frame_.size(); ++i) {
        put("\"");
        put(params[i].key);
        put("\":");
        // the ws api reads int params only as json numbers
        const bool bare = numeric_key(params[i].key);
        if (!bare) {
            put("\"");
        }
        put(params[i].value);
        if (!bare) {
            put("\"");
        }
        put(",");
    }
    put(R"("signature":")");
    put(signature);
    put(R"("}})");
    if (at > frame_.size()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return std::string_view{frame_.data(), at};
}

std::expected<WsApiReply, WireError> parse_ws_api_reply(std::string_view frame) {
    std::string_view error_raw;
    auto reply = wire::guarded([&]() -> std::expected<WsApiReply, WireError> {
        auto root = wire::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WsApiReply out{};
        const auto id = wire::opt_raw(*root, "id");
        if (!id.has_value()) {
            return std::unexpected(id.error());
        }
        auto digits = *id;
        if (digits.size() >= 2 && digits.front() == '"' && digits.back() == '"') {
            digits = digits.substr(1, digits.size() - 2);
        }
        out.id = wire::digits_to_u64(digits);
        const auto status = wire::opt_u64(*root, "status");
        if (!status.has_value()) {
            return std::unexpected(status.error());
        }
        out.status = static_cast<std::uint32_t>(*status);
        const auto error = wire::opt_raw(*root, "error");
        if (!error.has_value()) {
            return std::unexpected(error.error());
        }
        error_raw = *error;
        return out;
    });
    if (!reply.has_value() || error_raw.empty()) {
        return reply;
    }
    const auto error = parse_rest_error(error_raw);
    if (!error.has_value()) {
        return std::unexpected(error.error());
    }
    reply->error = *error;
    return reply;
}

std::optional<std::string> WsApiCodec::login(UnixNanos) {
    if (!signer_->has_credentials()) {
        return std::nullopt;
    }
    return std::string{};
}

net::WsRpcFrame WsApiCodec::classify(std::string_view payload) noexcept {
    const auto reply = parse_ws_api_reply(payload);
    if (!reply.has_value() || reply->id == 0) {
        return {};
    }
    return {.kind = net::WsRpcFrame::Kind::Reply, .id = reply->id, .ok = reply->status == 200, .detail = {}};
}

} // namespace raikiri::venues::binance
