#include <raikiri/net/json_writer.hpp>
#include <raikiri/venues/bitget/trade.hpp>
#include <raikiri/wire/json.hpp>

#include <charconv>
#include <cstring>
#include <span>

namespace raikiri::venues::bitget {

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

[[nodiscard]] std::string_view unquote(std::string_view raw) noexcept {
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
        return raw.substr(1, raw.size() - 2);
    }
    return raw;
}

} // namespace

std::expected<std::string_view, BuildError> TradeFrameBuilder::place(std::uint64_t id,
                                                                     const OrderCmd& cmd,
                                                                     const exec::InstrumentTable& table,
                                                                     std::string_view product,
                                                                     std::string_view inst_type) {
    const auto* instrument = table.find(cmd.instrument);
    if (instrument == nullptr) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    const auto& base = base_of(*instrument);
    net::JsonWriter json{params_};
    json.obj_begin();
    if (const auto fields = write_order_fields(json, cmd, base, product); !fields.has_value()) {
        return std::unexpected{fields.error()};
    }
    json.obj_end();
    if (!json.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return frame(id, inst_type, base.exchange_symbol.view(), k_channel_place_order, json.view());
}

std::expected<std::string_view, BuildError> TradeFrameBuilder::cancel(std::uint64_t id,
                                                                      const OrderCmd& cmd,
                                                                      const exec::InstrumentTable& table,
                                                                      std::string_view inst_type) {
    const auto symbol = table.exchange_symbol(cmd.instrument);
    if (symbol.empty()) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    net::JsonWriter json{params_};
    json.obj_begin();
    // orderId wins when both are sent
    if (!cmd.target_exchange_order_id.empty()) {
        json.str("orderId", cmd.target_exchange_order_id.view());
    } else {
        json.str("clientOid", cmd.exchange_client_order_id.view());
    }
    json.obj_end();
    if (!json.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return frame(id, inst_type, symbol, k_channel_cancel_order, json.view());
}

std::expected<std::string_view, BuildError> TradeFrameBuilder::frame(std::uint64_t id,
                                                                     std::string_view inst_type,
                                                                     std::string_view inst_id,
                                                                     std::string_view channel,
                                                                     std::string_view params) {
    if (inst_type.empty()) {
        return std::unexpected{BuildError::UnsupportedMarket};
    }
    Appender out{frame_};
    out << R"({"op":"trade","args":[{"id":")" << id << R"(","instType":")" << inst_type << R"(","instId":")" << inst_id
        << R"(","channel":")" << channel << R"(","params":)" << params << "}]}";
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
        const auto event = wire::opt_str(*root, "event");
        if (!event.has_value()) {
            return std::unexpected(event.error());
        }
        out.event = *event;
        const auto code = wire::opt_raw(*root, "code");
        if (!code.has_value()) {
            return std::unexpected(code.error());
        }
        out.code = unquote(*code);
        const auto msg = wire::opt_str(*root, "msg");
        if (!msg.has_value()) {
            return std::unexpected(msg.error());
        }
        out.msg = *msg;
        if (!wire::has_field(*root, "arg")) {
            return out;
        }
        auto args = wire::req_array(*root, "arg");
        if (!args.has_value()) {
            return std::unexpected(args.error());
        }
        for (auto element : *args) {
            wire::ondemand::object arg;
            if (element.get_object().get(arg) != simdjson::SUCCESS) {
                return std::unexpected(WireError::WrongType);
            }
            const auto id = wire::opt_str(arg, "id");
            if (!id.has_value()) {
                return std::unexpected(id.error());
            }
            out.id = wire::digits_to_u64(*id);
            break;
        }
        return out;
    });
}

std::optional<std::string> TradeCodec::login(UnixNanos now) {
    const auto venue_now = UnixNanos{static_cast<std::uint64_t>(static_cast<std::int64_t>(now.ns) + *clock_offset_ns_)};
    const auto frame = protocol_.login_frame(*signer_, passphrase_->view(), venue_now);
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
    if (reply->id != 0) {
        return {
            .kind = net::WsRpcFrame::Kind::Reply, .id = reply->id, .ok = reply->code == k_ws_ok, .detail = reply->msg};
    }
    if (reply->event == "login") {
        return {.kind = net::WsRpcFrame::Kind::Login, .id = 0, .ok = reply->code == k_ws_ok, .detail = reply->msg};
    }
    // a refused login comes back as a bare error event; the channel reads it only while logging in
    if (reply->event == "error") {
        return {.kind = net::WsRpcFrame::Kind::Login, .id = 0, .ok = false, .detail = reply->msg};
    }
    return {};
}

} // namespace raikiri::venues::bitget
