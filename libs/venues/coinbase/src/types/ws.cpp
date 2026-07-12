#include <raikiri/venues/coinbase/ws.hpp>

#include <algorithm>
#include <cstring>

namespace raikiri::venues::coinbase {

namespace {

constexpr std::string_view k_host = "ws-feed.exchange.coinbase.com";

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

std::string_view stream_host() noexcept {
    return k_host;
}

std::string_view stream_port() noexcept {
    return "443";
}

std::string_view stream_path() noexcept {
    return "/";
}

std::string_view channel_for(exec::SubKind kind) noexcept {
    switch (kind) {
    case exec::SubKind::Bbo:
        return "ticker";
    case exec::SubKind::Trades:
        return "matches";
    // The batched book: level2 proper needs credentials, level2_batch does
    // not. The ack renames it "level2_50" and delivers the whole book anyway.
    case exec::SubKind::Depth:
        return "level2_batch";
    // Everything below declines, each for a reason that will not change.
    case exec::SubKind::Funding:
    case exec::SubKind::MarkPrice: // no derivatives here, so no mark
    case exec::SubKind::Kline:     // no websocket candles on this API
        return {};
    }
    return {};
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::op_frame(std::string_view type, std::span<const Subscription> subscriptions) {
    if (subscriptions.empty()) {
        return std::unexpected{WsBuildError::NothingToSend};
    }
    const auto batch = subscriptions.size();
    for (std::size_t i = 0; i < batch; ++i) {
        if (subscriptions[i].channel.empty() || subscriptions[i].product_id.empty()) {
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
    put(R"({"type":")");
    put(type);
    put(R"(","channels":[)");
    std::size_t emitted = 0;
    for (std::size_t i = 0; i < batch; ++i) {
        const auto seen_before = [&] {
            for (std::size_t j = 0; j < i; ++j) {
                if (subscriptions[j].channel == subscriptions[i].channel) {
                    return true;
                }
            }
            return false;
        }();
        if (seen_before) {
            continue;
        }
        if (emitted != 0) {
            put(",");
        }
        put(R"({"name":")");
        put(subscriptions[i].channel);
        put(R"(","product_ids":[)");
        std::size_t products = 0;
        for (std::size_t j = i; j < batch; ++j) {
            if (subscriptions[j].channel != subscriptions[i].channel) {
                continue;
            }
            if (products != 0) {
                put(",");
            }
            put("\"");
            put(subscriptions[j].product_id);
            put("\"");
            ++products;
        }
        put("]}");
        ++emitted;
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

} // namespace raikiri::venues::coinbase
