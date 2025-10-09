#include <raikiri/venues/bybit/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::bybit {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
namespace ondemand = detail::ondemand;

} // namespace

// Bybit topics are "kind.arg[.arg]" — the symbol is always the last segment.
// Resolving the kind once here turns the per-message dispatch that follows
// into an integer compare instead of a chain of string compares at line rate.
TopicInfo classify_topic(std::string_view topic) noexcept {
    TopicInfo info{};
    const auto last_dot = topic.rfind('.');
    if (last_dot != std::string_view::npos) {
        info.symbol = topic.substr(last_dot + 1);
    }
    if (topic.starts_with("orderbook.")) {
        info.kind = TopicKind::Orderbook;
    } else if (topic.starts_with("publicTrade.")) {
        info.kind = TopicKind::PublicTrade;
    } else if (topic.starts_with("tickers.")) {
        info.kind = TopicKind::Tickers;
    } else if (topic == "order") {
        info.kind = TopicKind::Order;
        info.symbol = {};
    } else if (topic == "execution") {
        info.kind = TopicKind::Execution;
        info.symbol = {};
    } else if (topic == "position") {
        info.kind = TopicKind::Position;
        info.symbol = {};
    } else if (topic == "wallet") {
        info.kind = TopicKind::Wallet;
        info.symbol = {};
    } else {
        info.kind = TopicKind::Unknown;
        info.symbol = {};
    }
    return info;
}

std::expected<TopicInfo, WireError> parse_topic(std::string_view frame) {
    return guarded([&]() -> std::expected<TopicInfo, WireError> {
        auto& session = detail::session();
        auto root = session.open_object(frame);
        if (!root.has_value()) {
            return std::unexpected{root.error()};
        }
        auto obj = *root;

        if (detail::has_field(obj, "topic")) {
            auto topic = detail::req_str(obj, "topic");
            if (!topic.has_value()) {
                return std::unexpected{topic.error()};
            }
            return classify_topic(*topic);
        }
        obj.reset();
        const auto op = detail::opt_str(obj, "op");
        obj.reset();
        const auto ret_msg = detail::opt_str(obj, "ret_msg");
        const auto op_text = op.value_or(std::string_view{});
        const auto msg_text = ret_msg.value_or(std::string_view{});
        if (op_text == "pong" || msg_text == "pong") {
            return TopicInfo{.kind = TopicKind::Pong, .symbol = {}};
        }
        if (!op_text.empty() || detail::has_field(obj, "success")) {
            return TopicInfo{.kind = TopicKind::OpResponse, .symbol = {}};
        }
        return std::unexpected{WireError::UnknownMessage};
    });
}

std::expected<WireRestEnvelope, WireError> parse_envelope(std::string_view body) {
    return guarded([&]() -> std::expected<WireRestEnvelope, WireError> {
        auto& session = detail::session();
        auto root = session.open_object(body);
        if (!root.has_value()) {
            return std::unexpected{root.error()};
        }
        auto obj = *root;

        WireRestEnvelope out{};
        const auto code = detail::opt_i64(obj, "retCode");
        if (!code.has_value()) {
            return std::unexpected{code.error()};
        }
        out.ret_code = *code;
        obj.reset();
        out.ret_msg = detail::opt_str(obj, "retMsg").value_or(std::string_view{});
        obj.reset();
        out.time_ms = detail::opt_u64(obj, "time").value_or(0);
        obj.reset();
        out.result = detail::opt_raw(obj, "result").value_or(std::string_view{});
        return out;
    });
}

} // namespace raikiri::venues::bybit
