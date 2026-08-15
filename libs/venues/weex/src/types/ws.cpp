#include <raikiri/venues/weex/endpoints.hpp>
#include <raikiri/venues/weex/ws.hpp>

#include <algorithm>
#include <charconv>
#include <cstring>

namespace raikiri::venues::weex {

namespace {

[[nodiscard]] std::string_view suffix_for(exec::SubKind kind) noexcept {
    switch (kind) {
    case exec::SubKind::Bbo:
    case exec::SubKind::Depth:
        // ONE CHANNEL FOR BOTH. WEEX streams no quote of its own, so the top of
        // book is the ladder's first level rather than a second subscription.
        return k_depth_channel;
    case exec::SubKind::Trades:
        return k_trade_channel;
    case exec::SubKind::MarkPrice:
        // The ticker is where WEEX states a mark price, and the same frame
        // carries the 24-hour turnover.
        return k_ticker_channel;
    case exec::SubKind::Funding:
        // Not a channel at all: polled over REST. The data client routes this
        // kind before it reaches here.
        return {};
    case exec::SubKind::Kline:
        // WEEX streams klines as <sym>@kline_<interval>_LAST_PRICE; not wired
        // until a strategy asks for one.
        return {};
    }
    return {};
}

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
    return ep::k_stream_host;
}

std::string_view stream_port() noexcept {
    return ep::k_stream_port;
}

std::string_view stream_path() noexcept {
    return ep::k_ws_public;
}

std::string_view
channel_name(exec::SubKind kind, std::string_view exchange_symbol, std::span<char> out) noexcept {
    const auto suffix = suffix_for(kind);
    if (suffix.empty() || exchange_symbol.empty()) {
        return {};
    }
    if (exchange_symbol.size() + suffix.size() > out.size()) {
        return {};
    }
    std::memcpy(out.data(), exchange_symbol.data(), exchange_symbol.size());
    std::memcpy(out.data() + exchange_symbol.size(), suffix.data(), suffix.size());
    return {out.data(), exchange_symbol.size() + suffix.size()};
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::subscribe_frame(std::span<const std::string_view> channels) {
    return method_frame(R"({"method":"SUBSCRIBE","params":[)", channels);
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::unsubscribe_frame(std::span<const std::string_view> channels) {
    return method_frame(R"({"method":"UNSUBSCRIBE","params":[)", channels);
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::method_frame(std::string_view head, std::span<const std::string_view> channels) {
    if (channels.empty()) {
        return std::unexpected{WsBuildError::NothingToSend};
    }
    const auto batch = channels.size();
    for (std::size_t i = 0; i < batch; ++i) {
        // AN EMPTY NAME POISONS THE WHOLE FRAME. WEEX refuses a params array
        // for one bad member and subscribes none of the rest, so a channel
        // nothing could name is refused here rather than taking the others
        // down with it.
        if (channels[i].empty()) {
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
    put(head);
    for (std::size_t i = 0; i < batch; ++i) {
        if (i != 0) {
            put(",");
        }
        put("\"");
        put(channels[i]);
        put("\"");
    }
    put(R"(],"id":)");
    std::array<char, 24> id_buf{};
    const auto id = next_id_++;
    const auto [stop, ec] = std::to_chars(id_buf.data(), id_buf.data() + id_buf.size(), id);
    if (ec != std::errc{}) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    put({id_buf.data(), static_cast<std::size_t>(stop - id_buf.data())});
    put("}");
    if (overflow) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    return Frame{.text = {frame_.data(), len}, .consumed = batch};
}

} // namespace raikiri::venues::weex
