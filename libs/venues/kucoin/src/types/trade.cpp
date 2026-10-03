#include <raikiri/net/json_writer.hpp>
#include <raikiri/venues/kucoin/endpoints.hpp>
#include <raikiri/venues/kucoin/trade.hpp>
#include <raikiri/wire/json.hpp>

#include <charconv>
#include <cstring>
#include <span>

namespace raikiri::venues::kucoin {

namespace {

class Appender {
public:
    explicit Appender(std::span<char> buf) noexcept : buf_{buf} {}

    Appender& operator<<(std::string_view text) noexcept {
        if (overflow_ || len_ + text.size() > buf_.size()) {
            overflow_ = true;
            return *this;
        }
        text.copy(buf_.data() + len_, text.size());
        len_ += text.size();
        return *this;
    }

    Appender& operator<<(std::uint64_t value) noexcept {
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

// base64 in a query: only '+', '/' and '=' need escaping
void append_query_value(std::string& out, std::string_view base64) {
    for (const char c : base64) {
        switch (c) {
        case '+':
            out += "%2B";
            break;
        case '/':
            out += "%2F";
            break;
        case '=':
            out += "%3D";
            break;
        default:
            out += c;
        }
    }
}

} // namespace

std::expected<std::string_view, BuildError>
TradeFrameBuilder::place(std::uint64_t id, MarketType market, std::string_view body) {
    switch (market) {
    case MarketType::Spot:
        return frame(id, k_op_spot_order, body);
    case MarketType::Perp:
    case MarketType::Future:
        return frame(id, k_op_futures_order, body);
    case MarketType::Option:
        break;
    }
    return std::unexpected{BuildError::UnsupportedMarket};
}

std::expected<std::string_view, BuildError>
TradeFrameBuilder::cancel(std::uint64_t id, const OrderCmd& cmd, const exec::InstrumentTable& table) {
    const auto market = cmd.instrument.market();
    if (market == MarketType::Option) {
        return std::unexpected{BuildError::UnsupportedMarket};
    }
    const auto symbol = table.exchange_symbol(cmd.instrument);
    if (symbol.empty()) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    const auto by_order_id = !cmd.target_exchange_order_id.empty();
    const auto client_oid = cmd.exchange_client_order_id.view();
    if (!by_order_id && !client_oid_usable(client_oid)) {
        return std::unexpected{BuildError::BadClientOrderId};
    }
    net::JsonWriter json{args_};
    json.obj_begin();
    if (by_order_id) {
        json.str("orderId", cmd.target_exchange_order_id.view());
    } else {
        json.str("clientOid", client_oid);
    }
    json.str("symbol", symbol).obj_end();
    if (!json.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return frame(id, market == MarketType::Spot ? k_op_spot_cancel : k_op_futures_cancel, json.view());
}

std::expected<std::string_view, BuildError>
TradeFrameBuilder::frame(std::uint64_t id, std::string_view op, std::string_view args) {
    Appender out{frame_};
    out << R"({"id":")" << id << R"(","op":")" << op << R"(","args":)" << args << "}";
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
        // a string in every documented reply; a number reads the same
        const auto code = wire::opt_raw(*root, "code");
        if (!code.has_value()) {
            return std::unexpected(code.error());
        }
        auto digits = *code;
        if (digits.size() >= 2 && digits.front() == '"' && digits.back() == '"') {
            digits = digits.substr(1, digits.size() - 2);
        }
        out.code = static_cast<std::int64_t>(wire::digits_to_u64(digits));
        const auto msg = wire::opt_str(*root, "msg");
        if (!msg.has_value()) {
            return std::unexpected(msg.error());
        }
        out.msg = *msg;
        const auto data = wire::opt_raw(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        out.welcome = *data == R"("welcome")";
        out.session = !out.welcome && out.op.empty() && wire::has_field(*root, "sessionId");
        return out;
    });
}

std::string TradeCodec::target(UnixNanos now) const {
    if (!credentials_->complete()) {
        return {};
    }
    const auto& signer = credentials_->signer();
    std::array<char, 24> ms_text{};
    const auto [stop, ec] = std::to_chars(ms_text.data(), ms_text.data() + ms_text.size(), now.ns / 1'000'000ULL);
    if (ec != std::errc{}) {
        return {};
    }
    const auto timestamp = std::string_view{ms_text.data(), static_cast<std::size_t>(stop - ms_text.data())};
    std::string message{signer.api_key()};
    message += timestamp;
    std::array<char, net::k_base64_bytes> signature{};
    const auto sign = signer.sign(message, net::Signer::Encoding::Base64, signature);
    if (sign.empty()) {
        return {};
    }
    std::string out{ep::k_ws_trade_path};
    out += "?apikey=";
    out += signer.api_key();
    out += "&timestamp=";
    out += timestamp;
    out += "&sign=";
    append_query_value(out, sign);
    out += "&passphrase=";
    append_query_value(out, credentials_->sealed_passphrase());
    return out;
}

std::optional<std::string> TradeCodec::login(UnixNanos) {
    if (!credentials_->complete()) {
        return std::nullopt;
    }
    return std::string{};
}

std::optional<std::string> TradeCodec::answer(std::string_view challenge) {
    std::array<char, net::k_base64_bytes> signature{};
    const auto sign = credentials_->signer().sign(challenge, net::Signer::Encoding::Base64, signature);
    if (sign.empty()) {
        return std::nullopt;
    }
    return std::string{sign};
}

net::WsRpcFrame TradeCodec::classify(std::string_view payload) noexcept {
    const auto reply = parse_trade_reply(payload);
    if (!reply.has_value()) {
        return {};
    }
    if (reply->session) {
        return {.kind = net::WsRpcFrame::Kind::Challenge, .id = 0, .ok = true, .detail = {}};
    }
    if (reply->welcome) {
        return {.kind = net::WsRpcFrame::Kind::Login, .id = 0, .ok = true, .detail = {}};
    }
    if (reply->op == "pong") {
        return {.kind = net::WsRpcFrame::Kind::Pong, .id = 0, .ok = true, .detail = {}};
    }
    if (reply->id != 0) {
        return {.kind = net::WsRpcFrame::Kind::Reply, .id = reply->id, .ok = reply->code == k_ok, .detail = reply->msg};
    }
    // a refused connection answers with a bare code; the channel reads it only while logging in
    if (reply->code != 0 && reply->code != k_ok) {
        return {.kind = net::WsRpcFrame::Kind::Login, .id = 0, .ok = false, .detail = reply->msg};
    }
    return {};
}

} // namespace raikiri::venues::kucoin
