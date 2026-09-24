#include <raikiri/venues/bingx/endpoints.hpp>
#include <raikiri/venues/bingx/ws.hpp>

#include <charconv>
#include <cstring>
#include <string>

namespace raikiri::venues::bingx {

std::string_view to_string(WsBuildError err) noexcept {
    switch (err) {
    case WsBuildError::UnsupportedKind:
        return "UnsupportedKind";
    case WsBuildError::BufferTooSmall:
        return "BufferTooSmall";
    }
    return "Unknown";
}

std::string_view stream_host(MarketType market) noexcept {
    return market == MarketType::Spot ? ep::k_stream_spot : ep::k_stream_perp;
}

std::string_view stream_port() noexcept {
    return "443";
}

std::string_view stream_path(MarketType market) noexcept {
    return market == MarketType::Spot ? ep::k_ws_spot : ep::k_ws_perp;
}

std::string_view private_stream_host(MarketType market) noexcept {
    return stream_host(market);
}

std::string_view private_stream_path(MarketType market) noexcept {
    return stream_path(market);
}

std::string private_stream_target(MarketType market, std::string_view key) {
    std::string target{private_stream_path(market)};
    target += "?listenKey=";
    target.append(key);
    return target;
}

std::string_view data_type_suffix(exec::SubKind kind, MarketType market) noexcept {
    const bool spot = market == MarketType::Spot;
    switch (kind) {
    case exec::SubKind::Bbo:
        // the swap acks bookTicker but sends nothing, so the perp touch comes from the ladder
        return spot ? "bookTicker" : "depth100@100ms";
    case exec::SubKind::Trades:
        return "trade";
    case exec::SubKind::Depth:
        // 100 is the deepest tier; spot takes no interval suffix
        return spot ? "depth100" : "depth100@100ms";
    case exec::SubKind::MarkPrice:
        return spot ? std::string_view{} : "markPrice"; // no mark on a spot pair
    case exec::SubKind::Funding:
    case exec::SubKind::Kline:
        return {};
    }
    return {};
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::request(std::string_view req_type, std::string_view symbol, std::string_view suffix) {
    if (symbol.empty()) {
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
    if (!suffix.empty()) {
        put("@");
        put(suffix);
    }
    put(R"("})");
    if (overflow) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    return Frame{.text = {frame_.data(), len}};
}

std::expected<Protocol::Frame, WsBuildError> Protocol::subscribe_frame(std::string_view symbol,
                                                                       std::string_view suffix) {
    // kinds with no channel build no frame; an empty suffix is refused with 80015
    if (suffix.empty()) {
        return std::unexpected{WsBuildError::UnsupportedKind};
    }
    return request("sub", symbol, suffix);
}

std::expected<Protocol::Frame, WsBuildError> Protocol::unsubscribe_frame(std::string_view symbol,
                                                                         std::string_view suffix) {
    if (suffix.empty()) {
        return std::unexpected{WsBuildError::UnsupportedKind};
    }
    return request("unsub", symbol, suffix);
}

// no symbol: "spot.executionReport" is the whole dataType
std::expected<Protocol::Frame, WsBuildError>
Protocol::subscribe_channel(std::string_view data_type) {
    return request("sub", data_type, {});
}

} // namespace raikiri::venues::bingx
