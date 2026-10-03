#include <raikiri/venues/gate/trade.hpp>
#include <raikiri/wire/json.hpp>

#include <charconv>
#include <cstring>
#include <span>

namespace raikiri::venues::gate {

namespace {

// the rest builder's account; a unified account would want "unified" in both
constexpr std::string_view k_spot_account = "spot";

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

    Writer& operator<<(char) = delete;

    [[nodiscard]] bool ok() const noexcept { return !overflow_; }

    [[nodiscard]] std::string_view view() const noexcept { return {buf_.data(), len_}; }

private:
    std::span<char> buf_;
    std::size_t len_{0};
    bool overflow_{false};
};

std::uint64_t seconds_at(UnixNanos now, std::int64_t offset_ns) noexcept {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(now.ns) + offset_ns) / 1'000'000'000ULL;
}

} // namespace

std::string_view trade_channel(MarketType market, TradeOp op) noexcept {
    const bool spot = market == MarketType::Spot;
    if (!spot && market != MarketType::Perp) {
        return {};
    }
    switch (op) {
    case TradeOp::Login:
        return spot ? "spot.login" : "futures.login";
    case TradeOp::Place:
        return spot ? "spot.order_place" : "futures.order_place";
    case TradeOp::Cancel:
        return spot ? "spot.order_cancel" : "futures.order_cancel";
    case TradeOp::Amend:
        return spot ? "spot.order_amend" : "futures.order_amend";
    }
    return {};
}

std::expected<std::string_view, BuildError> TradeFrameBuilder::request(std::uint64_t id,
                                                                       MarketType market,
                                                                       TradeOp op,
                                                                       std::string_view target,
                                                                       std::string_view symbol,
                                                                       std::string_view body,
                                                                       std::uint64_t now_s) {
    const auto channel = trade_channel(market, op);
    if (channel.empty() || op == TradeOp::Login) {
        return std::unexpected{BuildError::OptionsUnsupported};
    }
    const bool spot = market == MarketType::Spot;
    Writer out{frame_};
    out << R"({"time":)" << now_s << R"(,"channel":")" << channel << R"(","event":"api","payload":{"req_id":")" << id
        << R"(","req_param":)";
    if (op == TradeOp::Place) {
        out << body;
    } else {
        if (target.empty() || (spot && symbol.empty())) {
            return std::unexpected{BuildError::NoOrderTarget};
        }
        out << R"({"order_id":")" << target << "\"";
        if (spot) {
            out << R"(,"currency_pair":")" << symbol << R"(","account":")" << k_spot_account << "\"";
        }
        if (op == TradeOp::Amend && body.size() > 2 && body.front() == '{') {
            out << "," << body.substr(1);
        } else {
            out << "}";
        }
    }
    out << "}}";
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
        const auto id = wire::opt_str(*root, "request_id");
        if (!id.has_value()) {
            return std::unexpected(id.error());
        }
        out.id = wire::digits_to_u64(*id);
        const auto ack = wire::opt_bool(*root, "ack");
        if (!ack.has_value()) {
            return std::unexpected(ack.error());
        }
        out.ack = *ack;
        if (!wire::has_field(*root, "header")) {
            const auto channel = wire::opt_str(*root, "channel");
            if (!channel.has_value()) {
                return std::unexpected(channel.error());
            }
            out.channel = *channel;
            return out;
        }
        auto header = wire::req_object(*root, "header");
        if (!header.has_value()) {
            return std::unexpected(header.error());
        }
        const auto channel = wire::opt_str(*header, "channel");
        const auto status = wire::opt_str(*header, "status");
        if (!channel.has_value() || !status.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.channel = *channel;
        out.status = static_cast<std::uint32_t>(wire::digits_to_u64(*status));
        if (!wire::has_field(*root, "data")) {
            return out;
        }
        auto data = wire::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        const auto result = wire::opt_raw(*data, "result");
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        out.result = *result;
        if (!wire::has_field(*data, "errs")) {
            return out;
        }
        auto errs = wire::req_object(*data, "errs");
        if (!errs.has_value()) {
            return std::unexpected(errs.error());
        }
        const auto label = wire::opt_str(*errs, "label");
        const auto message = wire::opt_str(*errs, "message");
        if (!label.has_value() || !message.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.label = *label;
        out.message = *message;
        return out;
    });
}

std::optional<std::string> TradeCodec::login(UnixNanos now) {
    const auto channel = trade_channel(market_, TradeOp::Login);
    if (!signer_->has_credentials() || channel.empty()) {
        return std::nullopt;
    }
    const auto now_s = seconds_at(now, *clock_offset_ns_);
    std::array<char, 64> message_buf{};
    Writer message{message_buf};
    message << "api\n" << channel << "\n\n" << now_s;
    if (!message.ok()) {
        return std::nullopt;
    }
    const auto signature = signer_->sign(message.view(), net::Signer::Encoding::Hex512, signature_);
    if (signature.empty()) {
        return std::nullopt;
    }
    std::array<char, 512> frame_buf{};
    Writer frame{frame_buf};
    frame << R"({"time":)" << now_s << R"(,"channel":")" << channel << R"(","event":"api","payload":{"api_key":")"
          << signer_->api_key() << R"(","signature":")" << signature << R"(","timestamp":")" << now_s
          << R"(","req_id":"0"}})";
    if (!frame.ok()) {
        return std::nullopt;
    }
    return std::string{frame.view()};
}

net::WsRpcFrame TradeCodec::classify(std::string_view payload) noexcept {
    const auto reply = parse_trade_reply(payload);
    if (!reply.has_value()) {
        return {};
    }
    const bool ok = reply->status == 200;
    if (reply->channel.ends_with(".login")) {
        return {.kind = net::WsRpcFrame::Kind::Login, .id = 0, .ok = ok, .detail = reply->label};
    }
    if (reply->ack || reply->id == 0) {
        return {};
    }
    return {.kind = net::WsRpcFrame::Kind::Reply, .id = reply->id, .ok = ok, .detail = reply->label};
}

} // namespace raikiri::venues::gate
