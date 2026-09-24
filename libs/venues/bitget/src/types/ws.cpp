#include <raikiri/venues/bitget/endpoints.hpp>
#include <raikiri/venues/bitget/ws.hpp>

#include <algorithm>
#include <charconv>
#include <cstring>

namespace raikiri::venues::bitget {

namespace {

constexpr std::string_view k_inst_type_live = "USDT-FUTURES";
constexpr std::string_view k_inst_type_demo = "SUSDT-FUTURES";
constexpr std::string_view k_inst_type_spot = "SPOT";

constexpr std::string_view k_login_method = "GET";
constexpr std::string_view k_login_path = "/user/verify";

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
        return "UnsupportedKind";
    case WsBuildError::NothingToSend:
        return "NothingToSend";
    case WsBuildError::BufferTooSmall:
        return "BufferTooSmall";
    case WsBuildError::NotSigned:
        return "NotSigned";
    }
    return "Unknown";
}

std::string_view stream_host() noexcept {
    return ep::stream_host();
}

std::string_view stream_port() noexcept {
    return "443";
}

std::string_view stream_path() noexcept {
    return ep::k_ws_public;
}

std::string_view private_stream_path() noexcept {
    return ep::k_ws_private;
}

std::string_view inst_type(MarketType market, bool testnet) noexcept {
    switch (market) {
    case MarketType::Spot:
        // demo is futures-only; spot has no S-prefixed instType
        return k_inst_type_spot;
    case MarketType::Perp:
    case MarketType::Future:
        // delivery lists under the same usdt-m product as perps
        return testnet ? k_inst_type_demo : k_inst_type_live;
    case MarketType::Option:
        // bitget lists no options; empty makes subscribe_frame refuse
        return {};
    }
    return {};
}

std::string_view channel_for(exec::SubKind kind) noexcept {
    switch (kind) {
    case exec::SubKind::Bbo:
    case exec::SubKind::Funding:
    case exec::SubKind::MarkPrice:
        return "ticker";
    case exec::SubKind::Trades:
        return "trade";
    case exec::SubKind::Depth:
        // books15 is the deepest snapshot channel; books50 and books100 are refused with 30016
        return "books15";
    case exec::SubKind::Kline:
        return {}; // the bar size is part of the channel name; no consumer yet
    }
    return {};
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::op_frame(std::string_view op, std::span<const Subscription> subscriptions) {
    if (subscriptions.empty()) {
        return std::unexpected{WsBuildError::NothingToSend};
    }
    const auto batch = subscriptions.size();
    for (std::size_t i = 0; i < batch; ++i) {
        // a declined kind or missing instType gets 30016
        if (subscriptions[i].channel.empty() || subscriptions[i].inst_id.empty() ||
            subscriptions[i].inst_type.empty()) {
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
        put(R"({"instType":")");
        put(subscriptions[i].inst_type);
        put(R"(","channel":")");
        put(subscriptions[i].channel);
        put(R"(","instId":")");
        put(subscriptions[i].inst_id);
        put(R"("})");
    }
    put("]}");
    if (overflow) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    return Frame{.text = {frame_.data(), len}, .consumed = batch};
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::subscribe_frame(std::span<const Subscription> subscriptions) {
    return op_frame("subscribe", subscriptions);
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::unsubscribe_frame(std::span<const Subscription> subscriptions) {
    return op_frame("unsubscribe", subscriptions);
}

std::string_view Protocol::last_login_message() const noexcept {
    return {login_msg_.data(), login_msg_len_};
}

std::expected<std::string_view, WsBuildError>
Protocol::login_frame(const net::Signer& signer, std::string_view passphrase, UnixNanos now) {
    if (!signer.has_credentials() || passphrase.empty()) {
        return std::unexpected{WsBuildError::NotSigned};
    }
    // the passphrase is sent unescaped, so quotes and backslashes are refused
    if (passphrase.find_first_of("\"\\") != std::string_view::npos) {
        return std::unexpected{WsBuildError::NotSigned};
    }

    std::array<char, 24> seconds_text{};
    const auto seconds = render_u64(now.ns / 1'000'000'000ULL, seconds_text);
    if (seconds.empty()) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }

    std::size_t len = 0;
    const auto append = [&](std::string_view text) {
        if (len + text.size() > login_msg_.size()) {
            return false;
        }
        std::memcpy(login_msg_.data() + len, text.data(), text.size());
        len += text.size();
        return true;
    };
    if (!append(seconds) || !append(k_login_method) || !append(k_login_path)) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    login_msg_len_ = len;

    const auto signature =
        signer.sign(last_login_message(), net::Signer::Encoding::Base64, signature_);
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
    put(R"({"op":"login","args":[{"apiKey":")");
    put(signer.api_key());
    put(R"(","passphrase":")");
    put(passphrase);
    put(R"(","timestamp":")");
    put(seconds); // a JSON string, and the same bytes the digest covered
    put(R"(","sign":")");
    put(signature);
    put(R"("}]})");
    if (overflow) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    return std::string_view{frame_.data(), out};
}

} // namespace raikiri::venues::bitget
