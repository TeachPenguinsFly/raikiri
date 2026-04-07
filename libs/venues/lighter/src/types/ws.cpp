#include <raikiri/venues/lighter/endpoints.hpp>
#include <raikiri/venues/lighter/ws.hpp>

#include <charconv>
#include <cstring>

namespace raikiri::venues::lighter {

namespace {

[[nodiscard]] std::string_view channel_for(exec::SubKind kind) noexcept {
    switch (kind) {
    case exec::SubKind::Bbo:
    case exec::SubKind::Depth:
        return k_book_channel;
    case exec::SubKind::Trades:
        return k_trade_channel;
    case exec::SubKind::Funding:
    case exec::SubKind::MarkPrice:
    case exec::SubKind::Kline:
        return {};
    }
    return {};
}

} // namespace

std::string_view to_string(WsBuildError err) noexcept {
    switch (err) {
    case WsBuildError::BufferTooSmall:
        return "BufferTooSmall";
    }
    return "Unknown";
}

std::string_view stream_host(bool testnet) noexcept {
    return ep::stream_host(testnet);
}

std::string_view stream_path() noexcept {
    return ep::k_ws_stream;
}

std::string_view
channel_name(exec::SubKind kind, std::uint32_t market_id, std::span<char> out) noexcept {
    const auto channel = channel_for(kind);
    if (channel.empty() || out.size() < channel.size() + 1) {
        return {};
    }
    std::memcpy(out.data(), channel.data(), channel.size());
    auto* cursor = out.data() + channel.size();
    *cursor++ = '/';
    const auto [stop, ec] = std::to_chars(cursor, out.data() + out.size(), market_id);
    if (ec != std::errc{}) {
        return {};
    }
    return {out.data(), static_cast<std::size_t>(stop - out.data())};
}

std::expected<std::string_view, WsBuildError> Protocol::subscribe_frame(std::string_view channel) {
    return frame("subscribe", channel);
}

std::expected<std::string_view, WsBuildError>
Protocol::unsubscribe_frame(std::string_view channel) {
    return frame("unsubscribe", channel);
}

std::expected<std::string_view, WsBuildError> Protocol::frame(std::string_view type,
                                                              std::string_view channel) {
    constexpr std::string_view k_open = R"({"type":")";
    constexpr std::string_view k_mid = R"(","channel":")";
    constexpr std::string_view k_close = R"("})";
    const auto size = k_open.size() + type.size() + k_mid.size() + channel.size() + k_close.size();
    if (size > frame_.size()) {
        return std::unexpected(WsBuildError::BufferTooSmall);
    }
    auto* cursor = frame_.data();
    for (const auto part : {k_open, type, k_mid, channel, k_close}) {
        std::memcpy(cursor, part.data(), part.size());
        cursor += part.size();
    }
    return std::string_view{frame_.data(), size};
}

} // namespace raikiri::venues::lighter
