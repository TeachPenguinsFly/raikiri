#include <raikiri/venues/okx/endpoints.hpp>
#include <raikiri/venues/okx/ws.hpp>

#include <algorithm>
#include <charconv>
#include <cstring>

namespace raikiri::venues::okx {

namespace {

// The literal OKX signs on login. It is not a path any request goes to and
// not a method — it is a fixed string that happens to look like both.
constexpr std::string_view k_login_prefix = "GET/users/self/verify";

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

std::string_view stream_host(bool testnet) noexcept {
    return ep::stream_host(testnet);
}

std::string_view stream_port() noexcept {
    return ep::k_stream_port;
}

std::string_view stream_path() noexcept {
    return ep::k_ws_public;
}

std::string_view private_stream_path() noexcept {
    return ep::k_ws_private;
}

std::string_view channel_for(exec::SubKind kind) noexcept {
    switch (kind) {
    case exec::SubKind::Bbo:
        return "bbo-tbt";
    case exec::SubKind::Trades:
        return "trades";
    case exec::SubKind::Depth:
        return "books";
    case exec::SubKind::Funding:
        return "funding-rate";
    case exec::SubKind::MarkPrice:
        return "mark-price";
    case exec::SubKind::Kline:
        return {}; // needs a bar size in the channel name; no consumer yet
    }
    return {};
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::op_frame(std::string_view op, std::span<const Subscription> subscriptions) {
    if (subscriptions.empty()) {
        return std::unexpected{WsBuildError::NothingToSend};
    }
    const auto batch = std::min(subscriptions.size(), k_max_subscriptions_per_frame);
    for (std::size_t i = 0; i < batch; ++i) {
        // One of the two identifies the target; neither is a subscription to
        // everything, which OKX would answer with 60018.
        if (subscriptions[i].channel.empty() ||
            (subscriptions[i].inst_id.empty() && subscriptions[i].inst_type.empty())) {
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
        put(R"({"channel":")");
        put(subscriptions[i].channel);
        put("\"");
        if (!subscriptions[i].inst_type.empty()) {
            put(R"(,"instType":")");
            put(subscriptions[i].inst_type);
            put("\"");
        }
        if (!subscriptions[i].inst_id.empty()) {
            put(R"(,"instId":")");
            put(subscriptions[i].inst_id);
            put("\"");
        }
        put("}");
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
    std::array<char, 24> seconds_text{};
    const auto seconds = now.ns / 1'000'000'000ULL;
    const auto [stop, ec] =
        std::to_chars(seconds_text.data(), seconds_text.data() + seconds_text.size(), seconds);
    if (ec != std::errc{}) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    const std::string_view timestamp{seconds_text.data(),
                                     static_cast<std::size_t>(stop - seconds_text.data())};

    std::size_t len = 0;
    const auto append = [&](std::string_view text) {
        if (len + text.size() > login_msg_.size()) {
            return false;
        }
        std::memcpy(login_msg_.data() + len, text.data(), text.size());
        len += text.size();
        return true;
    };
    if (!append(timestamp) || !append(k_login_prefix)) {
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
    // Every one of these is a JSON string, the timestamp included — the
    // unquoted form bybit demands is rejected here as 60012.
    put(R"(","timestamp":")");
    put(timestamp);
    put(R"(","sign":")");
    put(signature);
    put(R"("}]})");
    if (overflow) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    return std::string_view{frame_.data(), out};
}

} // namespace raikiri::venues::okx
