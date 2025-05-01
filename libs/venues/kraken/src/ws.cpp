#include <raikiri/venues/kraken/ws.hpp>

#include <algorithm>
#include <cstring>

namespace raikiri::venues::kraken {

namespace {

constexpr std::string_view k_host = "futures.kraken.com";

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
    return "/ws/v1";
}

std::string_view feed_for(exec::SubKind kind) noexcept {
    switch (kind) {
    case exec::SubKind::Bbo:
    case exec::SubKind::Funding:
    case exec::SubKind::MarkPrice:
        return "ticker";
    case exec::SubKind::Trades:
        return "trade";
    case exec::SubKind::Depth:
        return "book";
    case exec::SubKind::Kline:
        return {}; // Kraken serves candles over a separate REST charts API
    }
    return {};
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::event_frame(std::string_view event, std::span<const Subscription> subscriptions) {
    if (subscriptions.empty()) {
        return std::unexpected{WsBuildError::NothingToSend};
    }
    const auto feed = subscriptions.front().feed;
    if (feed.empty()) {
        return std::unexpected{WsBuildError::UnsupportedKind};
    }
    std::size_t batch = 0;
    while (batch < subscriptions.size() && batch < k_max_products_per_frame &&
           subscriptions[batch].feed == feed) {
        if (subscriptions[batch].product_id.empty()) {
            return std::unexpected{WsBuildError::UnsupportedKind};
        }
        ++batch;
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
    put(R"({"event":")");
    put(event);
    put(R"(","feed":")");
    put(feed);
    put(R"(","product_ids":[)");
    for (std::size_t i = 0; i < batch; ++i) {
        if (i != 0) {
            put(",");
        }
        put("\"");
        put(subscriptions[i].product_id);
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
    return event_frame("subscribe", subscriptions);
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::unsubscribe_frame(std::span<const Subscription> subscriptions) {
    return event_frame("unsubscribe", subscriptions);
}

} // namespace raikiri::venues::kraken
