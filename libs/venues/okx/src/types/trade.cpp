#include <raikiri/venues/okx/envelope.hpp>
#include <raikiri/venues/okx/trade.hpp>
#include <raikiri/wire/json.hpp>

#include <charconv>
#include <cstring>
#include <span>

namespace raikiri::venues::okx {

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
TradeFrameBuilder::request(std::uint64_t id, std::string_view op, std::string_view body, std::uint64_t inst_id_code) {
    if (inst_id_code == 0) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    if (body.size() < 2 || body.front() != '{') {
        return std::unexpected{BuildError::NothingToSend};
    }
    Writer out{frame_};
    out << R"({"id":")" << id << R"(","op":")" << op << R"(","args":[{"instIdCode":)" << inst_id_code << ","
        << body.substr(1) << "]}";
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
        const auto id = wire::opt_str(*root, "id");
        if (!id.has_value()) {
            return std::unexpected(id.error());
        }
        out.id = wire::digits_to_u64(*id);
        const auto op = wire::opt_str(*root, "op");
        if (!op.has_value()) {
            return std::unexpected(op.error());
        }
        out.op = *op;
        const auto event = wire::opt_str(*root, "event");
        if (!event.has_value()) {
            return std::unexpected(event.error());
        }
        out.event = *event;
        const auto code = wire::opt_str(*root, "code");
        if (!code.has_value()) {
            return std::unexpected(code.error());
        }
        out.code = static_cast<std::int64_t>(wire::digits_to_u64(*code));
        if (out.code == 0 && !code->empty() && *code != "0") {
            return std::unexpected(WireError::WrongType);
        }
        const auto msg = wire::opt_str(*root, "msg");
        if (!msg.has_value()) {
            return std::unexpected(msg.error());
        }
        out.msg = *msg;
        return out;
    });
}

std::optional<std::string> TradeCodec::login(UnixNanos now) {
    const UnixNanos venue_now{static_cast<std::uint64_t>(static_cast<std::int64_t>(now.ns) + *clock_offset_ns_)};
    const auto frame = protocol_.login_frame(*signer_, passphrase_, venue_now);
    if (!frame.has_value()) {
        return std::nullopt;
    }
    return std::string{*frame};
}

net::WsRpcFrame TradeCodec::classify(std::string_view payload) noexcept {
    if (payload == "pong") {
        return {.kind = net::WsRpcFrame::Kind::Pong, .id = 0, .ok = true, .detail = {}};
    }
    const auto reply = parse_trade_reply(payload);
    if (!reply.has_value()) {
        return {};
    }
    // a refused login answers event error, which the channel reads only while logging in
    if (reply->event == "login" || reply->event == "error") {
        return {.kind = net::WsRpcFrame::Kind::Login,
                .id = 0,
                .ok = reply->event == "login" && reply->code == k_ok,
                .detail = reply->msg};
    }
    if (reply->id == 0) {
        return {};
    }
    return {.kind = net::WsRpcFrame::Kind::Reply, .id = reply->id, .ok = reply->code == k_ok, .detail = reply->msg};
}

} // namespace raikiri::venues::okx
