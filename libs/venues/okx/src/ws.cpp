#include <raikiri/venues/okx/ws.hpp>

#include <algorithm>
#include <cstring>

namespace raikiri::venues::okx {

namespace {

constexpr std::string_view k_host_live = "ws.okx.com";
constexpr std::string_view k_host_demo = "wspap.okx.com";

} // namespace

std::string_view to_string(WsBuildError err) noexcept {
    switch (err) {
    case WsBuildError::UnsupportedKind:
        return "UnsupportedKind";
    case WsBuildError::NothingToSend:
        return "NothingToSend";
    case WsBuildError::BufferTooSmall:
        return "BufferTooSmall";
    }
    return "Unknown";
}

std::string_view stream_host(bool testnet) noexcept {
    return testnet ? k_host_demo : k_host_live;
}

std::string_view stream_port() noexcept {
    return "8443";
}

std::string_view stream_path() noexcept {
    return "/ws/v5/public";
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
        if (subscriptions[i].channel.empty() || subscriptions[i].inst_id.empty()) {
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

} // namespace raikiri::venues::okx
