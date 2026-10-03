#include <raikiri/venues/bybit/trade.hpp>
#include <raikiri/wire/json.hpp>

#include <charconv>
#include <cstring>

namespace raikiri::venues::bybit {

namespace {

class Writer {
public:
    explicit Writer(std::span<char> buf) noexcept : buf_{buf} {}

    Writer& operator<<(std::string_view text) noexcept {
        if (overflow_ || len_ + text.size() > buf_.size()) {
            overflow_ = true;
            return *this;
        }
        text.copy(buf_.data() + len_, text.size());
        len_ += text.size();
        return *this;
    }

    Writer& operator<<(std::uint64_t value) noexcept {
        std::array<char, 24> digits{};
        const auto [stop, ec] = std::to_chars(digits.data(), digits.data() + digits.size(), value);
        if (ec != std::errc{}) {
            overflow_ = true;
            return *this;
        }
        return *this << std::string_view{digits.data(), static_cast<std::size_t>(stop - digits.data())};
    }

    [[nodiscard]] bool ok() const noexcept { return !overflow_; }

    [[nodiscard]] std::string_view view() const noexcept { return {buf_.data(), len_}; }

private:
    std::span<char> buf_;
    std::size_t len_{0};
    bool overflow_{false};
};

} // namespace

std::expected<std::string_view, BuildError>
TradeFrameBuilder::request(std::uint64_t id, std::string_view op, std::string_view body, UnixNanos now) {
    Writer out{frame_};
    out << R"({"reqId":")" << id << R"(","header":{"X-BAPI-TIMESTAMP":")" << now.ns / 1'000'000ULL
        << R"(","X-BAPI-RECV-WINDOW":")" << std::uint64_t{recv_window_ms_} << R"("},"op":")" << op << R"(","args":[)"
        << body << "]}";
    if (!out.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return out.view();
}

std::expected<WireTradeReply, WireError> parse_trade_reply(std::string_view frame) {
    return wire::guarded([&]() -> std::expected<WireTradeReply, WireError> {
        auto root = wire::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireTradeReply out{};
        const auto req_id = wire::opt_str(*root, "reqId");
        if (!req_id.has_value()) {
            return std::unexpected(req_id.error());
        }
        out.req_id = wire::digits_to_u64(*req_id);
        const auto code = wire::opt_i64(*root, "retCode");
        if (!code.has_value()) {
            return std::unexpected(code.error());
        }
        out.ret_code = *code;
        const auto msg = wire::opt_str(*root, "retMsg");
        if (!msg.has_value()) {
            return std::unexpected(msg.error());
        }
        out.ret_msg = *msg;
        const auto op = wire::opt_str(*root, "op");
        if (!op.has_value()) {
            return std::unexpected(op.error());
        }
        out.op = *op;
        return out;
    });
}

std::optional<std::string> TradeCodec::login(UnixNanos now) {
    const auto frame = protocol_.auth_frame(*signer_, now);
    if (!frame.has_value()) {
        return std::nullopt;
    }
    return std::string{*frame};
}

net::WsRpcFrame TradeCodec::classify(std::string_view payload) noexcept {
    const auto reply = parse_trade_reply(payload);
    if (!reply.has_value()) {
        return {};
    }
    if (reply->op == "auth") {
        return {
            .kind = net::WsRpcFrame::Kind::Login, .id = 0, .ok = reply->ret_code == k_ret_ok, .detail = reply->ret_msg};
    }
    if (reply->op == "pong") {
        return {.kind = net::WsRpcFrame::Kind::Pong, .id = 0, .ok = true, .detail = {}};
    }
    if (reply->req_id == 0) {
        return {};
    }
    return {.kind = net::WsRpcFrame::Kind::Reply,
            .id = reply->req_id,
            .ok = reply->ret_code == k_ret_ok,
            .detail = reply->ret_msg};
}

} // namespace raikiri::venues::bybit
