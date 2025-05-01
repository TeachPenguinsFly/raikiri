#include <raikiri/venues/mexc/ws.hpp>

#include <charconv>
#include <cstring>

namespace raikiri::venues::mexc {

namespace {

constexpr std::string_view k_host = "contract.mexc.com";

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
    return "/edge";
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

std::expected<Protocol::Frame, WsBuildError> Protocol::op_frame(std::string_view op,
                                                                const Subscription& subscription) {
    // Empty means channel_for() declined the kind.
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
        const auto [stop, ec] =
            std::to_chars(num.data(), num.data() + num.size(), subscription.limit);
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

std::expected<Protocol::Frame, WsBuildError>
Protocol::subscribe_frame(std::span<const Subscription> subscriptions) {
    if (subscriptions.empty()) {
        return std::unexpected{WsBuildError::NothingToSend};
    }
    return op_frame("sub", subscriptions.front());
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::unsubscribe_frame(std::span<const Subscription> subscriptions) {
    if (subscriptions.empty()) {
        return std::unexpected{WsBuildError::NothingToSend};
    }
    return op_frame("unsub", subscriptions.front());
}

} // namespace raikiri::venues::mexc
