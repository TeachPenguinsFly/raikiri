#include <raikiri/venues/kucoin/ws.hpp>

#include <algorithm>
#include <charconv>
#include <cstring>

namespace raikiri::venues::kucoin {

namespace {

constexpr std::string_view k_scheme = "wss://";

// the token goes on the request line unencoded; refuse anything that would break the query
[[nodiscard]] bool safe_in_query(std::string_view text) noexcept {
    return !text.empty() && std::ranges::none_of(text, [](char c) {
        return c <= ' ' || c == '#' || c == '&' || c == '?' || c == '"' || c == '\\';
    });
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
    case WsBuildError::BadEndpoint:
        return "BadEndpoint";
    }
    return "Unknown";
}

std::string_view topic_for(exec::SubKind kind, MarketType market) noexcept {
    const bool spot = market == MarketType::Spot;
    switch (kind) {
    case exec::SubKind::Bbo:
        // perp keeps tickerV2: a classic book without a rest seed has no touch
        // spot takes level1, since the spot ticker's time is the last print's, not the quote's
        return spot ? "/spotMarket/level1" : "/contractMarket/tickerV2";
    case exec::SubKind::Trades:
        return spot ? "/market/match" : "/contractMarket/execution";
    case exec::SubKind::Depth:
        // spot uses the depth-50 snapshot topic; both topics carry subject level2
        return spot ? "/spotMarket/level2Depth50" : "/contractMarket/level2";
    case exec::SubKind::Funding:
    case exec::SubKind::MarkPrice:
        // /contract/instrument is futures only
        return spot ? std::string_view{} : "/contract/instrument";
    case exec::SubKind::Kline:
        return {}; // needs a bar size in the topic; no consumer yet
    }
    return {};
}

std::string_view private_order_topic(MarketType market) noexcept {
    switch (market) {
    case MarketType::Spot:
        return "/spotMarket/tradeOrders";
    case MarketType::Perp:
    case MarketType::Future:
        return "/contractMarket/tradeOrders";
    case MarketType::Option:
        return {}; // KuCoin lists no options on the APIs in scope
    }
    return {};
}

std::expected<StreamEndpoint, WsBuildError>
make_endpoint(std::string_view endpoint_url, std::string_view token, std::string_view connect_id) {
    if (!endpoint_url.starts_with(k_scheme) || !safe_in_query(token) || !safe_in_query(connect_id)) {
        return std::unexpected{WsBuildError::BadEndpoint};
    }
    const auto authority_and_path = endpoint_url.substr(k_scheme.size());
    const auto slash = authority_and_path.find('/');
    const auto authority = slash == std::string_view::npos ? authority_and_path : authority_and_path.substr(0, slash);
    const auto path = slash == std::string_view::npos ? std::string_view{"/"} : authority_and_path.substr(slash);
    if (authority.empty()) {
        return std::unexpected{WsBuildError::BadEndpoint};
    }

    StreamEndpoint out{};
    const auto colon = authority.find(':');
    out.host = std::string{authority.substr(0, colon == std::string_view::npos ? authority.size() : colon)};
    out.port = colon == std::string_view::npos ? "443" : std::string{authority.substr(colon + 1)};
    out.target.reserve(path.size() + token.size() + connect_id.size() + 24);
    out.target.append(path).append("?token=").append(token).append("&connectId=").append(connect_id);
    return out;
}

std::expected<Protocol::Frame, WsBuildError> Protocol::op_frame(std::string_view type,
                                                                std::span<const Subscription> subscriptions) {
    if (subscriptions.empty()) {
        return std::unexpected{WsBuildError::NothingToSend};
    }
    const auto topic = subscriptions.front().topic;
    if (topic.empty()) {
        return std::unexpected{WsBuildError::UnsupportedKind};
    }
    std::size_t batch = 0;
    while (batch < subscriptions.size() && batch < k_max_symbols_per_topic && subscriptions[batch].topic == topic) {
        if (subscriptions[batch].symbol.empty()) {
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
        text.copy(frame_.data() + len, text.size());
        len += text.size();
    };
    std::array<char, 24> id{};
    const auto [stop, ec] = std::to_chars(id.data(), id.data() + id.size(), next_id_++);
    if (ec != std::errc{}) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    put(R"({"id":)");
    put(std::string_view{id.data(), static_cast<std::size_t>(stop - id.data())});
    put(R"(,"type":")");
    put(type);
    put(R"(","topic":")");
    put(topic);
    for (std::size_t i = 0; i < batch; ++i) {
        put(i == 0 ? ":" : ",");
        put(subscriptions[i].symbol);
    }
    // response:true asks for the ack
    put(R"(","response":true})");
    if (overflow) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    return Frame{.text = {frame_.data(), len}, .consumed = batch};
}

std::expected<Protocol::Frame, WsBuildError> Protocol::subscribe_frame(std::span<const Subscription> subscriptions) {
    return op_frame("subscribe", subscriptions);
}

std::expected<Protocol::Frame, WsBuildError> Protocol::unsubscribe_frame(std::span<const Subscription> subscriptions) {
    return op_frame("unsubscribe", subscriptions);
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::uta_subscribe_frame(std::span<const Subscription> subscriptions) {
    if (subscriptions.empty()) {
        return std::unexpected{WsBuildError::NothingToSend};
    }
    const auto& one = subscriptions.front();
    const bool book = one.topic == k_uta_book_channel;
    if (one.symbol.empty() || (!book && one.topic != k_uta_trade_channel)) {
        return std::unexpected{WsBuildError::UnsupportedKind};
    }
    std::size_t len = 0;
    bool overflow = false;
    const auto put = [&](std::string_view text) {
        if (overflow || len + text.size() > frame_.size()) {
            overflow = true;
            return;
        }
        text.copy(frame_.data() + len, text.size());
        len += text.size();
    };
    std::array<char, 24> id{};
    const auto [stop, ec] = std::to_chars(id.data(), id.data() + id.size(), next_id_++);
    if (ec != std::errc{}) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    // the id is a string here and the verb is action
    put(R"({"id":")");
    put(std::string_view{id.data(), static_cast<std::size_t>(stop - id.data())});
    put(R"(","action":"SUBSCRIBE","channel":")");
    put(one.topic);
    put(R"(","tradeType":"FUTURES","symbol":")");
    put(one.symbol);
    if (book) {
        put(R"(","depth":")");
        put(uta_book_depth_);
    }
    put(R"("})");
    if (overflow) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    return Frame{.text = std::string_view{frame_.data(), len}, .consumed = 1};
}

std::expected<std::string_view, WsBuildError> Protocol::private_subscribe_frame(std::string_view topic) {
    if (topic.empty()) {
        return std::unexpected{WsBuildError::UnsupportedKind};
    }
    std::size_t len = 0;
    bool overflow = false;
    const auto put = [&](std::string_view text) {
        if (overflow || len + text.size() > frame_.size()) {
            overflow = true;
            return;
        }
        text.copy(frame_.data() + len, text.size());
        len += text.size();
    };
    std::array<char, 24> id{};
    const auto [stop, ec] = std::to_chars(id.data(), id.data() + id.size(), next_id_++);
    if (ec != std::errc{}) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    put(R"({"id":)");
    put(std::string_view{id.data(), static_cast<std::size_t>(stop - id.data())});
    put(R"(,"type":"subscribe","topic":")");
    put(topic);
    // sent as a boolean; kucoin's examples show the string "true"
    put(R"(","privateChannel":true,"response":true})");
    if (overflow) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    return std::string_view{frame_.data(), len};
}

std::string_view Protocol::ping_frame() noexcept {
    std::array<char, 24> id{};
    const auto [stop, ec] = std::to_chars(id.data(), id.data() + id.size(), next_id_++);
    if (ec != std::errc{}) {
        return {};
    }
    constexpr std::string_view k_head = R"({"id":)";
    constexpr std::string_view k_tail = R"(,"type":"ping"})";
    const auto digits = static_cast<std::size_t>(stop - id.data());
    std::memcpy(ping_.data(), k_head.data(), k_head.size());
    std::memcpy(ping_.data() + k_head.size(), id.data(), digits);
    std::memcpy(ping_.data() + k_head.size() + digits, k_tail.data(), k_tail.size());
    return {ping_.data(), k_head.size() + digits + k_tail.size()};
}

} // namespace raikiri::venues::kucoin
