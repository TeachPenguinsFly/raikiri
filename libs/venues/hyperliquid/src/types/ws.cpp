#include <raikiri/venues/hyperliquid/ws.hpp>

#include <cstring>

namespace raikiri::venues::hyperliquid {

namespace {

constexpr std::string_view k_host_live = "api.hyperliquid.xyz";
constexpr std::string_view k_host_testnet = "api.hyperliquid-testnet.xyz";

} // namespace

std::string_view to_string(WsBuildError err) noexcept {
    switch (err) {
    case WsBuildError::UnsupportedKind:
        return "UnsupportedKind";
    case WsBuildError::BufferTooSmall:
        return "BufferTooSmall";
    }
    return "Unknown";
}

std::string_view stream_host(bool testnet) noexcept {
    return testnet ? k_host_testnet : k_host_live;
}

std::string_view stream_port() noexcept {
    return "443";
}

std::string_view stream_path() noexcept {
    return "/ws";
}

std::string_view channel_for(exec::SubKind kind) noexcept {
    switch (kind) {
    case exec::SubKind::Bbo:
        // Tick-by-tick, so no local book is needed to produce a BBO.
        return "bbo";
    case exec::SubKind::Trades:
        return "trades";
    case exec::SubKind::Depth:
        return "l2Book";
    case exec::SubKind::Funding:
    case exec::SubKind::MarkPrice:
        return "activeAssetCtx";
    case exec::SubKind::Kline:
        return {}; // `candle` needs an interval in the subscription; no consumer
    }
    return {};
}

std::expected<std::string_view, WsBuildError>
Protocol::method_frame(std::string_view method, const Subscription& subscription) {
    if (subscription.channel.empty() || subscription.coin.empty()) {
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
    put(method);
    put(R"(","subscription":{"type":")");
    put(subscription.channel);
    put(R"(","coin":")");
    put(subscription.coin);
    put(R"("}})");
    if (overflow) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    return std::string_view{frame_.data(), len};
}

std::expected<std::string_view, WsBuildError>
Protocol::subscribe_frame(const Subscription& subscription) {
    return method_frame("subscribe", subscription);
}

std::expected<std::string_view, WsBuildError>
Protocol::unsubscribe_frame(const Subscription& subscription) {
    return method_frame("unsubscribe", subscription);
}

} // namespace raikiri::venues::hyperliquid
