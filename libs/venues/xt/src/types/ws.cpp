
#include <raikiri/venues/xt/endpoints.hpp>
#include <raikiri/venues/xt/ws.hpp>

#include <algorithm>
#include <charconv>
#include <cstring>

namespace raikiri::venues::xt {

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

std::string_view stream_host(MarketType market) noexcept {
    // Two deployments, one name each: spot's public socket is the same host
    // as its private one, and so is the futures tree's.
    return market == MarketType::Spot ? ep::k_stream_spot : ep::k_stream_futures;
}

std::string_view stream_port() noexcept {
    return "443";
}

std::string_view stream_path(MarketType market) noexcept {
    return market == MarketType::Spot ? ep::k_ws_spot_public : ep::k_ws_futures_market;
}

Topic topic_for(exec::SubKind kind, MarketType market) noexcept {
    const bool spot = market == MarketType::Spot;
    switch (kind) {
    case exec::SubKind::Bbo:
    case exec::SubKind::Depth:
        // ONE CHANNEL ANSWERS BOTH. `depth` is a self-contained top-N snapshot
        // and the mapper publishes a BBO and a whole book from each frame, so
        // asking for the book asks for this. It used to name `depth_update`
        // instead, which chains deltas nothing here applies: the frames were
        // subscribed, arrived, and fell into the ignore arm.
        //
        // 50 is XT's ceiling on both trees — 60, 80, 99, 100 and 200 are each
        // refused with `levels error`. Spot takes no interval: ",50,100ms"
        // there is `args error`.
        return {.channel = "depth", .params = spot ? ",50" : ",50,100ms"};
    case exec::SubKind::Trades:
        return {.channel = "trade", .params = {}};
    case exec::SubKind::MarkPrice:
        // A spot pair has no mark price, and the spot tree refuses the topic.
        return spot ? Topic{} : Topic{.channel = "mark_price", .params = {}};
    case exec::SubKind::Funding:
        return {}; // no such topic; polled over REST
    case exec::SubKind::Kline:
        return {}; // needs a bar size in the topic name; no consumer yet
    }
    return {};
}

Topic ticker_topic() noexcept {
    return {.channel = "ticker", .params = {}};
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::method_frame(std::string_view method, std::span<const Subscription> subscriptions) {
    if (subscriptions.empty()) {
        return std::unexpected{WsBuildError::NothingToSend};
    }
    const auto batch = std::min(subscriptions.size(), k_max_subscriptions_per_frame);
    for (std::size_t i = 0; i < batch; ++i) {
        if (subscriptions[i].channel.empty() || subscriptions[i].symbol.empty()) {
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
    put(R"({"method":")");
    put(method);
    put(R"(","params":[)");
    for (std::size_t i = 0; i < batch; ++i) {
        if (i != 0) {
            put(",");
        }
        put("\"");
        put(subscriptions[i].channel);
        put("@");
        put(subscriptions[i].symbol);
        put(subscriptions[i].params);
        put("\"");
    }
    put(R"(],"id":")");
    std::array<char, 24> id{};
    if (const auto [stop, ec] = std::to_chars(id.data(), id.data() + id.size(), next_id_);
        ec == std::errc{}) {
        put(std::string_view{id.data(), static_cast<std::size_t>(stop - id.data())});
    }
    put(R"("})");
    if (overflow) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    ++next_id_;
    return Frame{.text = {frame_.data(), len}, .consumed = batch};
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::subscribe_frame(std::span<const Subscription> subscriptions) {
    return method_frame("subscribe", subscriptions);
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::unsubscribe_frame(std::span<const Subscription> subscriptions) {
    return method_frame("unsubscribe", subscriptions);
}

std::string_view private_stream_host() noexcept {
    return ep::k_stream_futures;
}

std::string_view private_stream_path() noexcept {
    return ep::k_ws_futures_user;
}

std::string_view spot_private_stream_host() noexcept {
    return ep::k_stream_spot;
}

std::string_view spot_private_stream_path() noexcept {
    return ep::k_ws_spot_private;
}

std::string_view PrivateProtocol::ping_frame() noexcept {
    return "ping";
}

std::expected<std::string_view, WsBuildError>
PrivateProtocol::futures_subscribe(std::string_view listen_key) {
    if (listen_key.empty()) {
        return std::unexpected{WsBuildError::NothingToSend};
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
    constexpr std::array<std::string_view, 2> k_topics{"order", "trade"};
    put(R"({"method":"SUBSCRIBE","params":[)");
    for (std::size_t i = 0; i < k_topics.size(); ++i) {
        if (i != 0) {
            put(",");
        }
        put("\"");
        put(k_topics[i]);
        put("@");
        put(listen_key);
        put("\"");
    }
    put(R"(],"id":")");
    std::array<char, 24> id{};
    if (const auto [stop, ec] = std::to_chars(id.data(), id.data() + id.size(), next_id_);
        ec == std::errc{}) {
        put(std::string_view{id.data(), static_cast<std::size_t>(stop - id.data())});
    }
    put(R"("})");
    if (overflow) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    ++next_id_;
    return std::string_view{frame_.data(), len};
}

std::expected<std::string_view, WsBuildError>
PrivateProtocol::spot_subscribe(std::string_view access_token) {
    if (access_token.empty()) {
        return std::unexpected{WsBuildError::NothingToSend};
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
    // The topics carry NO key suffix here; the key rides beside them.
    put(R"({"method":"subscribe","params":["order","trade"],"listenKey":")");
    put(access_token);
    put(R"(","id":")");
    std::array<char, 24> id{};
    if (const auto [stop, ec] = std::to_chars(id.data(), id.data() + id.size(), next_id_);
        ec == std::errc{}) {
        put(std::string_view{id.data(), static_cast<std::size_t>(stop - id.data())});
    }
    put(R"("})");
    if (overflow) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    ++next_id_;
    return std::string_view{frame_.data(), len};
}

} // namespace raikiri::venues::xt
