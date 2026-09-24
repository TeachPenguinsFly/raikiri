#include <raikiri/venues/bitfinex/ws.hpp>

#include <charconv>
#include <cstring>

namespace raikiri::venues::bitfinex {

namespace {

constexpr std::string_view k_host = "api-pub.bitfinex.com";

constexpr std::string_view k_deriv_prefix = "deriv:";

// p0 is the finest aggregation, f0 real time; len takes 1, 25, 100 or 250
constexpr std::string_view k_book_precision = "P0";
constexpr std::string_view k_book_frequency = "F0";
constexpr std::string_view k_book_length = "100";

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
    return "/ws/2";
}

ChannelKind channel_kind(std::string_view channel, std::string_view key) noexcept {
    if (channel == "ticker") {
        return ChannelKind::Ticker;
    }
    if (channel == "trades") {
        return ChannelKind::Trades;
    }
    if (channel == "book") {
        return ChannelKind::Book;
    }
    if (channel == "status") {
        return key.starts_with(k_deriv_prefix) ? ChannelKind::DerivStatus : ChannelKind::Unknown;
    }
    return ChannelKind::Unknown;
}

std::string_view channel_name(ChannelKind kind) noexcept {
    switch (kind) {
    case ChannelKind::Ticker:
        return "ticker";
    case ChannelKind::Trades:
        return "trades";
    case ChannelKind::Book:
        return "book";
    case ChannelKind::DerivStatus:
        return "status";
    case ChannelKind::Unknown:
        break;
    }
    return {};
}

ChannelKind kind_for(exec::SubKind kind) noexcept {
    switch (kind) {
    case exec::SubKind::Bbo:
        return ChannelKind::Ticker;
    case exec::SubKind::Trades:
        return ChannelKind::Trades;
    case exec::SubKind::Depth:
        return ChannelKind::Book;
    case exec::SubKind::Funding:
    case exec::SubKind::MarkPrice:
        return ChannelKind::DerivStatus;
    case exec::SubKind::Kline:
        return ChannelKind::Unknown; // needs a bar size in the key; no consumer yet
    }
    return ChannelKind::Unknown;
}

std::string_view symbol_from_deriv_key(std::string_view key) noexcept {
    if (!key.starts_with(k_deriv_prefix)) {
        return {};
    }
    key.remove_prefix(k_deriv_prefix.size());
    return key;
}

std::string_view Protocol::conf_frame() noexcept {
    return R"({"event":"conf","flags":32768})";
}

std::expected<Protocol::Frame, WsBuildError> Protocol::subscribe_frame(const Subscription& sub) {
    const auto channel = channel_name(sub.kind);
    if (channel.empty() || sub.symbol.empty()) {
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
    put(R"({"event":"subscribe","channel":")");
    put(channel);
    if (sub.kind == ChannelKind::DerivStatus) {
        put(R"(","key":")");
        put(k_deriv_prefix);
        put(sub.symbol);
        put(R"("})");
    } else {
        put(R"(","symbol":")");
        put(sub.symbol);
        if (sub.kind == ChannelKind::Book) {
            put(R"(","prec":")");
            put(k_book_precision);
            put(R"(","freq":")");
            put(k_book_frequency);
            put(R"(","len":")");
            put(k_book_length);
        }
        put(R"("})");
    }
    if (overflow) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    return Frame{.text = {frame_.data(), len}};
}

std::expected<Protocol::Frame, WsBuildError> Protocol::unsubscribe_frame(std::int64_t chan_id) {
    if (chan_id == 0) {
        return std::unexpected{WsBuildError::NothingToSend};
    }
    constexpr std::string_view k_head = R"({"event":"unsubscribe","chanId":)";
    std::array<char, 24> digits{};
    const auto [stop, ec] = std::to_chars(digits.data(), digits.data() + digits.size(), chan_id);
    if (ec != std::errc{}) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    const std::string_view number{digits.data(), static_cast<std::size_t>(stop - digits.data())};
    if (k_head.size() + number.size() + 1 > frame_.size()) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    std::memcpy(frame_.data(), k_head.data(), k_head.size());
    std::memcpy(frame_.data() + k_head.size(), number.data(), number.size());
    frame_[k_head.size() + number.size()] = '}';
    return Frame{.text = {frame_.data(), k_head.size() + number.size() + 1}};
}

} // namespace raikiri::venues::bitfinex
