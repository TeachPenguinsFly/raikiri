#include <raikiri/net/json_writer.hpp>
#include <raikiri/venues/bybit/endpoints.hpp>
#include <raikiri/venues/bybit/ws.hpp>

#include <charconv>
#include <cstring>

namespace raikiri::venues::bybit {

namespace {

// The literal bybit requires in the private handshake. It is not a path and
// not a method — it is a fixed string that happens to look like both.
constexpr std::string_view k_auth_prefix = "GET/realtime";

std::string_view render_u64(std::uint64_t value, std::span<char> out) noexcept {
    const auto [end, ec] = std::to_chars(out.data(), out.data() + out.size(), value);
    return ec == std::errc{}
               ? std::string_view{out.data(), static_cast<std::size_t>(end - out.data())}
               : std::string_view{};
}

} // namespace

std::string_view to_string(WsBuildError err) noexcept {
    switch (err) {
    case WsBuildError::UnsupportedKind:
        return "unsupported subscription kind";
    case WsBuildError::UnknownInstrument:
        return "unknown instrument";
    case WsBuildError::TooManyTopics:
        return "too many topics for one frame";
    case WsBuildError::BufferTooSmall:
        return "buffer too small";
    case WsBuildError::NotSigned:
        return "credentials not loaded";
    }
    return "unknown";
}

std::string_view public_stream_path(MarketType market) noexcept {
    return market == MarketType::Spot ? ep::k_ws_public_spot : ep::k_ws_public_linear;
}

std::string_view private_stream_path() noexcept {
    return ep::k_ws_private;
}

std::string_view host(bool testnet) noexcept {
    return ep::stream_host(testnet);
}

std::string_view
topic_name(exec::SubKind kind, std::string_view exchange_symbol, std::span<char> out) noexcept {
    std::string_view prefix;
    switch (kind) {
    case exec::SubKind::Bbo:
    case exec::SubKind::Depth:
        prefix = "orderbook.";
        break;
    case exec::SubKind::Trades:
        prefix = "publicTrade.";
        break;
    case exec::SubKind::Funding:
    case exec::SubKind::MarkPrice:
        prefix = "tickers.";
        break;
    case exec::SubKind::Kline:
        return {}; // offered, but nothing here consumes it yet
    }

    std::size_t len = 0;
    const auto put = [&](std::string_view text) {
        if (len + text.size() > out.size()) {
            len = out.size() + 1; // mark overflow
            return;
        }
        std::memcpy(out.data() + len, text.data(), text.size());
        len += text.size();
    };
    put(prefix);
    if (prefix == "orderbook.") {
        char depth[8];
        put(render_u64(k_book_depth, depth));
        put(".");
    }
    put(exchange_symbol);
    return len > out.size() ? std::string_view{} : std::string_view{out.data(), len};
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::op_frame(std::string_view op, std::span<const std::string_view> topics) {
    if (topics.empty()) {
        return std::unexpected{WsBuildError::TooManyTopics};
    }
    const auto batch = std::min(topics.size(), k_max_topics_per_frame);
    for (std::size_t i = 0; i < batch; ++i) {
        if (topics[i].empty()) {
            return std::unexpected{WsBuildError::UnsupportedKind};
        }
    }

    std::size_t len = 0;
    bool overflow = false;
    const auto put = [&](std::string_view text) {
        if (overflow || len + text.size() > frame_.size()) {
            overflow = true;
            return;
        }
        std::memcpy(frame_.data() + len, text.data(), text.size());
        len += text.size();
    };
    put(R"({"op":")");
    put(op);
    put(R"(","args":[)");
    for (std::size_t i = 0; i < batch; ++i) {
        if (i != 0) {
            put(",");
        }
        put("\"");
        put(topics[i]);
        put("\"");
    }
    put("]}");
    if (overflow) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    return Frame{.text = {frame_.data(), len}, .consumed = batch};
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::subscribe_frame(std::span<const std::string_view> topics) {
    return op_frame("subscribe", topics);
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::unsubscribe_frame(std::span<const std::string_view> topics) {
    return op_frame("unsubscribe", topics);
}

std::string_view Protocol::last_auth_message() const noexcept {
    return {auth_msg_.data(), auth_msg_len_};
}

std::expected<std::string_view, WsBuildError>
Protocol::auth_frame(const net::Signer& signer, UnixNanos now, std::uint32_t validity_ms) {
    if (!signer.has_credentials()) {
        return std::unexpected{WsBuildError::NotSigned};
    }
    // The expiry must be ahead of bybit's clock when the frame lands.
    const auto expires_ms = now.ns / 1'000'000ULL + validity_ms;

    std::array<char, 24> expires_text{};
    const auto expires = render_u64(expires_ms, expires_text);

    std::size_t len = 0;
    const auto append = [&](std::string_view text) {
        if (len + text.size() > auth_msg_.size()) {
            return false;
        }
        std::memcpy(auth_msg_.data() + len, text.data(), text.size());
        len += text.size();
        return true;
    };
    if (!append(k_auth_prefix) || !append(expires)) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    auth_msg_len_ = len;

    const auto signature = signer.sign(last_auth_message(), net::Signer::Encoding::Hex, signature_);
    if (signature.empty()) {
        return std::unexpected{WsBuildError::NotSigned};
    }

    std::size_t out = 0;
    bool overflow = false;
    const auto put = [&](std::string_view text) {
        if (overflow || out + text.size() > frame_.size()) {
            overflow = true;
            return;
        }
        std::memcpy(frame_.data() + out, text.data(), text.size());
        out += text.size();
    };
    put(R"({"op":"auth","args":[")");
    put(signer.api_key());
    put("\",");
    put(expires); // a JSON number, not a string: bybit rejects the quoted form
    put(",\"");
    put(signature);
    put("\"]}");
    if (overflow) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    return std::string_view{frame_.data(), out};
}

} // namespace raikiri::venues::bybit
