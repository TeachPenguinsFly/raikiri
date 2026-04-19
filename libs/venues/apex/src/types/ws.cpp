#include <raikiri/venues/apex/ws.hpp>

#include <cstring>

namespace raikiri::venues::apex {

std::string_view to_string(WsBuildError err) noexcept {
    switch (err) {
    case WsBuildError::Empty:
        return "no topics";
    case WsBuildError::TooManyTopics:
        return "too many topics for one frame";
    case WsBuildError::BufferTooSmall:
        return "buffer too small";
    }
    return "unknown";
}

std::string_view
topic_name(exec::SubKind kind, std::string_view cross_symbol, std::span<char> out) noexcept {
    std::string_view prefix;
    switch (kind) {
    case exec::SubKind::Bbo:
    case exec::SubKind::Depth:
        prefix = k_book_prefix;
        break;
    case exec::SubKind::Trades:
        prefix = k_trade_prefix;
        break;
    case exec::SubKind::Funding:
    case exec::SubKind::MarkPrice:
        prefix = k_instrument_prefix;
        break;
    case exec::SubKind::Kline:
        return {};
    }
    if (cross_symbol.empty() || prefix.size() + cross_symbol.size() > out.size()) {
        return {};
    }
    std::memcpy(out.data(), prefix.data(), prefix.size());
    std::memcpy(out.data() + prefix.size(), cross_symbol.data(), cross_symbol.size());
    return {out.data(), prefix.size() + cross_symbol.size()};
}

std::expected<std::string_view, WsBuildError>
Protocol::op_frame(std::string_view op, std::span<const std::string_view> args) {
    if (args.empty()) {
        return std::unexpected{WsBuildError::Empty};
    }
    if (args.size() > k_max_topics_per_frame) {
        return std::unexpected{WsBuildError::TooManyTopics};
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
    put(R"({"op":")");
    put(op);
    put(R"(","args":[)");
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i != 0) {
            put(",");
        }
        put("\"");
        put(args[i]);
        put("\"");
    }
    put("]}");
    if (overflow) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    return std::string_view{frame_.data(), len};
}

std::expected<std::string_view, WsBuildError>
Protocol::subscribe_frame(std::span<const std::string_view> topics) {
    return op_frame("subscribe", topics);
}

std::expected<std::string_view, WsBuildError>
Protocol::unsubscribe_frame(std::span<const std::string_view> topics) {
    return op_frame("unsubscribe", topics);
}

std::expected<std::string_view, WsBuildError> Protocol::pong_frame(std::string_view value) {
    const std::array<std::string_view, 1> args{value};
    return op_frame("pong", args);
}

} // namespace raikiri::venues::apex
