#include <raikiri/venues/bingx/ws.hpp>

#include <charconv>
#include <cstring>

namespace raikiri::venues::bingx {

namespace {

constexpr std::string_view k_host = "open-api-swap.bingx.com";
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

std::string_view stream_host() noexcept {
    return k_host;
}

std::string_view stream_port() noexcept {
    return "443";
}

std::string_view stream_path() noexcept {
    return "/swap-market";
}

std::string_view data_type_suffix(exec::SubKind kind) noexcept {
    switch (kind) {
    case exec::SubKind::Bbo:
        return "bookTicker";
    case exec::SubKind::Trades:
        return "trade";
    case exec::SubKind::Depth:
        // 100 is BingX's deepest tier: depth200 is refused outright with
        // 80015 "dataType not support" rather than downgraded.
        return "depth100@100ms";
    case exec::SubKind::MarkPrice:
        return "markPrice";
    case exec::SubKind::Funding:
    case exec::SubKind::Kline:
        return {};
    }
    return {};
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::request(std::string_view req_type, std::string_view symbol, std::string_view suffix) {
    if (symbol.empty() || suffix.empty()) {
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

    std::array<char, 24> id{};
    const auto [stop, ec] = std::to_chars(id.data(), id.data() + id.size(), ++next_id_);
    if (ec != std::errc{}) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    put(R"({"id":")");
    put(std::string_view{id.data(), static_cast<std::size_t>(stop - id.data())});
    put(R"(","reqType":")");
    put(req_type);
    put(R"(","dataType":")");
    put(symbol);
    put("@");
    put(suffix);
    put(R"("})");
    if (overflow) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    return Frame{.text = {frame_.data(), len}};
}

std::expected<Protocol::Frame, WsBuildError> Protocol::subscribe_frame(std::string_view symbol,
                                                                       std::string_view suffix) {
    return request("sub", symbol, suffix);
}

std::expected<Protocol::Frame, WsBuildError> Protocol::unsubscribe_frame(std::string_view symbol,
                                                                         std::string_view suffix) {
    return request("unsub", symbol, suffix);
}

} // namespace raikiri::venues::bingx
