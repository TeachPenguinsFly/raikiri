#include <raikiri/venues/bitmex/ws.hpp>

#include <algorithm>
#include <cstring>

namespace raikiri::venues::bitmex {

namespace {

constexpr std::string_view k_host_live = "ws.bitmex.com";
constexpr std::string_view k_host_test = "ws.testnet.bitmex.com";

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
    return testnet ? k_host_test : k_host_live;
}

std::string_view stream_port() noexcept {
    return "443";
}

std::string_view stream_path() noexcept {
    return "/realtime";
}

std::string_view topic_for(exec::SubKind kind) noexcept {
    switch (kind) {
    case exec::SubKind::Bbo:
        // Top of book without maintaining a local book — but one row per
        // liquidity pool, which map.hpp's QuoteBook is what reconciles.
        return "quote";
    case exec::SubKind::Trades:
        return "trade";
    case exec::SubKind::Depth:
        return "orderBookL2_25";
    case exec::SubKind::Funding:
    case exec::SubKind::MarkPrice:
        // One table carries both, unlike okx. The desired set is deduplicated
        // by (table, symbol), so asking for each in turn opens one subscription.
        return "instrument";
    case exec::SubKind::Kline:
        return {}; // tradeBinNm — needs a bar size in the name; no consumer yet
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
        // Empty means topic_for() declined the kind. An empty symbol would
        // subscribe to every contract on the venue, which is worse than an
        // error: it succeeds.
        if (subscriptions[i].table.empty() || subscriptions[i].symbol.empty()) {
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
        put(subscriptions[i].table);
        put(":");
        put(subscriptions[i].symbol);
        put("\"");
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

} // namespace raikiri::venues::bitmex
