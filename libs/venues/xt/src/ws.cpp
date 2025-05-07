#include <raikiri/venues/xt/ws.hpp>

#include <algorithm>
#include <charconv>
#include <cstring>

namespace raikiri::venues::xt {

namespace {

constexpr std::string_view k_host = "fstream.xt.com";

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
    return "/ws/market";
}

Topic topic_for(exec::SubKind kind) noexcept {
    switch (kind) {
    case exec::SubKind::Bbo:
        // Also the book: `depth_update` is subscribed but not applied, so this
        // snapshot is the only thing that reaches shm. 50 is XT's ceiling —
        // 60, 80, 99, 100 and 200 are each refused with `levels error`.
        return {.channel = "depth", .params = ",50,100ms"};
    case exec::SubKind::Trades:
        return {.channel = "trade", .params = {}};
    case exec::SubKind::Depth:
        return {.channel = "depth_update", .params = {}};
    case exec::SubKind::MarkPrice:
        return {.channel = "mark_price", .params = {}};
    case exec::SubKind::Funding:
        return {}; // no such topic; polled over REST
    case exec::SubKind::Kline:
        return {}; // needs a bar size in the topic name; no consumer yet
    }
    return {};
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

} // namespace raikiri::venues::xt
