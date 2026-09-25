#include <raikiri/venues/mexc/endpoints.hpp>
#include <raikiri/venues/mexc/ws.hpp>

#include <array>
#include <charconv>
#include <cstring>

namespace raikiri::venues::mexc {

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

// spot is a separate deployment: json in, protobuf out, string subscriptions
std::string_view stream_host(MarketType market) noexcept {
    return market == MarketType::Spot ? ep::k_stream_spot : ep::k_stream_futures;
}

std::string_view stream_port() noexcept {
    return "443";
}

std::string_view stream_path(MarketType market) noexcept {
    return market == MarketType::Spot ? ep::k_ws_spot : ep::k_ws_futures;
}

std::string_view channel_for(exec::SubKind kind) noexcept {
    switch (kind) {
    case exec::SubKind::Bbo:
        return "depth.full";
    case exec::SubKind::Trades:
        return "deal";
    case exec::SubKind::Depth:
        return "depth";
    case exec::SubKind::Funding:
    case exec::SubKind::MarkPrice:
        return "ticker";
    case exec::SubKind::Kline:
        return {}; // needs an interval in the param; no consumer yet
    }
    return {};
}

std::string_view spot_channel_for(exec::SubKind kind) noexcept {
    // ".pb" selects protobuf; the json channels are refused as "Blocked!"
    switch (kind) {
    case exec::SubKind::Bbo:
        return "spot@public.aggre.bookTicker.v3.api.pb@100ms@";
    case exec::SubKind::Trades:
        return "spot@public.aggre.deals.v3.api.pb@100ms@";
    case exec::SubKind::Depth:
        return "spot@public.limit.depth.v3.api.pb@";
    case exec::SubKind::Funding:
    case exec::SubKind::MarkPrice:
        return {}; // a spot pair has neither
    case exec::SubKind::Kline:
        return {};
    }
    return {};
}

std::expected<Protocol::Frame, WsBuildError> Protocol::op_frame(std::string_view op, const Subscription& subscription) {
    if (subscription.channel.empty() || subscription.symbol.empty()) {
        return std::unexpected{WsBuildError::UnsupportedKind};
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
    put(R"({"method":")");
    put(op);
    put(".");
    put(subscription.channel);
    put(R"(","param":{"symbol":")");
    put(subscription.symbol);
    put(R"(")");
    if (subscription.limit != 0) {
        put(R"(,"limit":)");
        std::array<char, 16> num{};
        const auto [stop, ec] = std::to_chars(num.data(), num.data() + num.size(), subscription.limit);
        if (ec != std::errc{}) {
            return std::unexpected{WsBuildError::BufferTooSmall};
        }
        put(std::string_view{num.data(), static_cast<std::size_t>(stop - num.data())});
    }
    put("}}");
    if (overflow) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    return Frame{.text = {frame_.data(), len}, .consumed = 1};
}

// one channel per frame, since a failed subscription is reported by id
std::expected<Protocol::Frame, WsBuildError> Protocol::spot_op_frame(std::string_view op,
                                                                     const Subscription& subscription) {
    if (subscription.channel.empty() || subscription.symbol.empty()) {
        return std::unexpected{WsBuildError::UnsupportedKind};
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
    put(R"({"method":")");
    put(op);
    put(R"(","params":[")");
    put(subscription.channel);
    put(subscription.symbol);
    if (subscription.limit != 0) {
        put("@");
        std::array<char, 16> num{};
        const auto [stop, ec] = std::to_chars(num.data(), num.data() + num.size(), subscription.limit);
        if (ec != std::errc{}) {
            return std::unexpected{WsBuildError::BufferTooSmall};
        }
        put(std::string_view{num.data(), static_cast<std::size_t>(stop - num.data())});
    }
    put(R"("]})");
    if (overflow) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    return Frame{.text = {frame_.data(), len}, .consumed = 1};
}

std::expected<Protocol::Frame, WsBuildError> Protocol::spot_subscribe_frame(const Subscription& subscription) {
    return spot_op_frame("SUBSCRIPTION", subscription);
}

std::expected<Protocol::Frame, WsBuildError> Protocol::subscribe_frame(std::span<const Subscription> subscriptions) {
    if (subscriptions.empty()) {
        return std::unexpected{WsBuildError::NothingToSend};
    }
    return op_frame("sub", subscriptions.front());
}

std::expected<Protocol::Frame, WsBuildError> Protocol::unsubscribe_frame(std::span<const Subscription> subscriptions) {
    if (subscriptions.empty()) {
        return std::unexpected{WsBuildError::NothingToSend};
    }
    return op_frame("unsub", subscriptions.front());
}

std::string_view Protocol::last_login_message() const noexcept {
    return {login_msg_.data(), login_msg_len_};
}

std::expected<std::string_view, WsBuildError> Protocol::login_frame(const net::Signer& signer, UnixNanos now) {
    if (!signer.has_credentials()) {
        return std::unexpected{WsBuildError::NotSigned};
    }
    std::array<char, 24> ts_buf{};
    const auto [ts_stop, ts_ec] = std::to_chars(ts_buf.data(), ts_buf.data() + ts_buf.size(), now.ns / 1'000'000ULL);
    if (ts_ec != std::errc{}) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    const std::string_view ts{ts_buf.data(), static_cast<std::size_t>(ts_stop - ts_buf.data())};

    std::size_t msg_len = 0;
    bool msg_overflow = false;
    const auto put_msg = [&](std::string_view text) {
        if (msg_overflow || msg_len + text.size() > login_msg_.size()) {
            msg_overflow = true;
            return;
        }
        std::memcpy(login_msg_.data() + msg_len, text.data(), text.size());
        msg_len += text.size();
    };
    put_msg(signer.api_key());
    put_msg(ts);
    if (msg_overflow) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    login_msg_len_ = msg_len;

    const auto signature = signer.sign(
        std::string_view{login_msg_.data(), msg_len}, net::Signer::Encoding::Hex, login_sig_);
    if (signature.empty()) {
        return std::unexpected{WsBuildError::NotSigned};
    }

    std::size_t len = 0;
    bool overflow = false;
    const auto put = [&](std::string_view text) {
        if (overflow || len + text.size() > login_frame_.size()) {
            overflow = true;
            return;
        }
        std::memcpy(login_frame_.data() + len, text.data(), text.size());
        len += text.size();
    };
    put(R"({"method":"login","param":{"apiKey":")");
    put(signer.api_key());
    // reqTime is a json string here; a number gets an error that reads as a bad key
    put(R"(","reqTime":")");
    put(ts);
    put(R"(","signature":")");
    put(signature);
    put(R"("}})");
    if (overflow) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    return std::string_view{login_frame_.data(), len};
}

} // namespace raikiri::venues::mexc
