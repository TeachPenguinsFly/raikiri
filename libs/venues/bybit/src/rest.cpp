#include <raikiri/venues/bybit/rest.hpp>

#include <raikiri/net/json_writer.hpp>

#include <charconv>
#include <cstring>

namespace raikiri::venues::bybit {

namespace {

// Header names are bybit's, capitalisation and all.
constexpr std::string_view k_hdr_key = "X-BAPI-API-KEY";
constexpr std::string_view k_hdr_ts = "X-BAPI-TIMESTAMP";
constexpr std::string_view k_hdr_sign = "X-BAPI-SIGN";
constexpr std::string_view k_hdr_recv = "X-BAPI-RECV-WINDOW";

std::string_view render_u64(std::uint64_t value, std::span<char> out) noexcept {
    const auto [end, ec] = std::to_chars(out.data(), out.data() + out.size(), value);
    return ec == std::errc{} ? std::string_view{out.data(), static_cast<std::size_t>(end - out.data())}
                             : std::string_view{};
}

// Appends into a fixed buffer, reporting overflow rather than truncating: a
// truncated signed body authenticates as something we did not mean to send.
class Appender {
public:
    explicit Appender(std::span<char> buf) noexcept : buf_{buf} {}

    Appender& operator<<(std::string_view text) noexcept {
        if (overflow_ || len_ + text.size() > buf_.size()) {
            overflow_ = true;
            return *this;
        }
        std::memcpy(buf_.data() + len_, text.data(), text.size());
        len_ += text.size();
        return *this;
    }

    [[nodiscard]] bool ok() const noexcept { return !overflow_; }
    [[nodiscard]] std::string_view view() const noexcept { return {buf_.data(), len_}; }

private:
    std::span<char> buf_;
    std::size_t len_{0};
    bool overflow_{false};
};

[[nodiscard]] std::string_view side_text(Side side) noexcept {
    return side == Side::Buy ? "Buy" : "Sell";
}

// Post-only is a TIME IN FORCE value on bybit, not a separate boolean the way
// most venues spell it. Sending postOnly as a flag is silently ignored, and
// the order crosses.
[[nodiscard]] std::string_view tif_text(TimeInForce tif, OrderFlags flags) noexcept {
    if (has_flag(flags, OrderFlags::PostOnly)) {
        return "PostOnly";
    }
    switch (tif) {
    case TimeInForce::Ioc:
        return "IOC";
    case TimeInForce::Fok:
        return "FOK";
    case TimeInForce::Gtc:
        break;
    }
    return "GTC";
}

// Settlement coin scopes the account-wide pulls. Without it bybit demands a
// symbol and returns an error rather than everything, which turns a
// reconciliation sweep into a per-symbol loop.
constexpr std::string_view k_settle_coin = "USDT";

} // namespace

std::string_view to_string(BuildError err) noexcept {
    switch (err) {
    case BuildError::UnknownInstrument:
        return "unknown instrument";
    case BuildError::BadQuantity:
        return "bad quantity";
    case BuildError::BadPrice:
        return "bad price";
    case BuildError::BufferTooSmall:
        return "buffer too small";
    case BuildError::NotSigned:
        return "credentials not loaded";
    }
    return "unknown";
}

std::string_view category_of(MarketType market) noexcept {
    return market == MarketType::Spot ? "spot" : "linear";
}

std::string_view RequestBuilder::last_sign_message() const noexcept {
    return {sign_msg_.data(), sign_msg_len_};
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::signed_post(std::string_view target, std::string_view body, UnixNanos now) {
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NotSigned};
    }
    const auto ts = render_u64(now.ns / 1'000'000ULL, timestamp_);
    const auto recv = render_u64(recv_window_ms_, recv_window_text_);

    // Bare concatenation, no separators. The body must be byte-identical to
    // what is actually sent — signing a re-serialised copy is how signatures
    // drift from payloads.
    Appender msg{sign_msg_};
    msg << ts << signer_->api_key() << recv << body;
    if (!msg.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    sign_msg_len_ = msg.view().size();

    const auto signature = signer_->sign(msg.view(), net::Signer::Encoding::Hex, signature_);
    if (signature.empty()) {
        return std::unexpected{BuildError::NotSigned};
    }

    net::RestRequest request{};
    request.method = net::HttpMethod::Post;
    request.target = target;
    request.body = body;
    request.headers = {{k_hdr_key, signer_->api_key()},
                       {k_hdr_ts, ts},
                       {k_hdr_recv, recv},
                       {k_hdr_sign, signature}};
    return request;
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::signed_get(std::string_view path, std::string_view query, UnixNanos now) {
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NotSigned};
    }
    const auto ts = render_u64(now.ns / 1'000'000ULL, timestamp_);
    const auto recv = render_u64(recv_window_ms_, recv_window_text_);

    // GET signs the QUERY STRING in place of the body, and without its
    // leading '?'. This asymmetry with POST is the single most common way to
    // produce a signature bybit rejects as a generic auth failure.
    Appender msg{sign_msg_};
    msg << ts << signer_->api_key() << recv << query;
    if (!msg.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    sign_msg_len_ = msg.view().size();

    const auto signature = signer_->sign(msg.view(), net::Signer::Encoding::Hex, signature_);
    if (signature.empty()) {
        return std::unexpected{BuildError::NotSigned};
    }

    Appender target{target_};
    target << path << "?" << query;
    if (!target.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }

    net::RestRequest request{};
    request.method = net::HttpMethod::Get;
    request.target = target.view();
    request.body = {};
    request.headers = {{k_hdr_key, signer_->api_key()},
                       {k_hdr_ts, ts},
                       {k_hdr_recv, recv},
                       {k_hdr_sign, signature}};
    return request;
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::create_order(const OrderCmd& cmd, const exec::InstrumentTable& table,
                             UnixNanos now) {
    const auto* instrument = table.find(cmd.instrument);
    if (instrument == nullptr) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    const auto& base = base_of(*instrument);
    const auto symbol = table.exchange_symbol(cmd.instrument);

    net::JsonWriter json{body_};
    json.obj_begin()
        .str("category", category_of(cmd.instrument.market()))
        .str("symbol", symbol)
        .str("side", side_text(cmd.side))
        .str("orderType", cmd.order_type == OrderType::Market ? "Market" : "Limit")
        // Quantities and prices go out as decimal strings rendered straight
        // from fixed-point, never through a float.
        .decimal("qty", base.lots_to_qty(cmd.qty));
    if (cmd.order_type == OrderType::Limit) {
        json.decimal("price", base.ticks_to_price(cmd.price));
    }
    json.str("timeInForce", tif_text(cmd.tif, cmd.flags))
        .str("orderLinkId", cmd.exchange_client_order_id.view());
    if (has_flag(cmd.flags, OrderFlags::ReduceOnly)) {
        json.boolean("reduceOnly", true);
    }
    json.obj_end();

    if (!json.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return signed_post("/v5/order/create", json.view(), now);
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::cancel_order(const OrderCmd& cmd, const exec::InstrumentTable& table,
                             UnixNanos now) {
    const auto symbol = table.exchange_symbol(cmd.instrument);
    if (symbol.empty()) {
        return std::unexpected{BuildError::UnknownInstrument};
    }

    net::JsonWriter json{body_};
    json.obj_begin()
        .str("category", category_of(cmd.instrument.market()))
        .str("symbol", symbol);
    // Prefer the venue's own id when we have it. Bybit resolves orderLinkId
    // only within 30 minutes of creation on some products, so an exchange id
    // is the more durable handle whenever the ack has already bound one.
    if (!cmd.target_exchange_order_id.empty()) {
        json.str("orderId", cmd.target_exchange_order_id.view());
    } else {
        json.str("orderLinkId", cmd.exchange_client_order_id.view());
    }
    json.obj_end();

    if (!json.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return signed_post("/v5/order/cancel", json.view(), now);
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::amend_order(const OrderCmd& cmd, const exec::InstrumentTable& table,
                            UnixNanos now) {
    const auto* instrument = table.find(cmd.instrument);
    if (instrument == nullptr) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    const auto& base = base_of(*instrument);
    const auto symbol = table.exchange_symbol(cmd.instrument);

    net::JsonWriter json{body_};
    json.obj_begin()
        .str("category", category_of(cmd.instrument.market()))
        .str("symbol", symbol);
    if (!cmd.target_exchange_order_id.empty()) {
        json.str("orderId", cmd.target_exchange_order_id.view());
    } else {
        json.str("orderLinkId", cmd.exchange_client_order_id.view());
    }
    // Bybit amends only the fields present, so sending an unset price would
    // rewrite it to zero rather than leave it alone.
    if (!cmd.qty.is_undef()) {
        json.decimal("qty", base.lots_to_qty(cmd.qty));
    }
    if (!cmd.price.is_undef()) {
        json.decimal("price", base.ticks_to_price(cmd.price));
    }
    json.obj_end();

    if (!json.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return signed_post("/v5/order/amend", json.view(), now);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::cancel_all(MarketType market,
                                                                       UnixNanos now) {
    net::JsonWriter json{body_};
    json.obj_begin().str("category", category_of(market)).str("settleCoin", k_settle_coin).obj_end();
    if (!json.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return signed_post("/v5/order/cancel-all", json.view(), now);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::open_orders(MarketType market,
                                                                        UnixNanos now) {
    Appender query{body_}; // reuse the body buffer: GET sends none
    query << "category=" << category_of(market) << "&settleCoin=" << k_settle_coin;
    if (!query.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return signed_get("/v5/order/realtime", query.view(), now);
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::executions(MarketType market, UnixNanos since, UnixNanos now) {
    std::array<char, 24> since_text{};
    const auto since_ms = render_u64(since.ns / 1'000'000ULL, since_text);

    Appender query{body_};
    query << "category=" << category_of(market);
    if (since.ns != 0) {
        query << "&startTime=" << since_ms;
    }
    if (!query.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return signed_get("/v5/execution/list", query.view(), now);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::positions(MarketType market,
                                                                       UnixNanos now) {
    Appender query{body_};
    query << "category=" << category_of(market) << "&settleCoin=" << k_settle_coin;
    if (!query.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return signed_get("/v5/position/list", query.view(), now);
}

net::RestRequest RequestBuilder::instruments(MarketType market) {
    Appender target{target_};
    target << "/v5/market/instruments-info?category=" << category_of(market) << "&limit=1000";

    net::RestRequest request{};
    request.method = net::HttpMethod::Get;
    request.target = target.view();
    // No credentials: the catalogue is public, and attaching a signature
    // would only risk leaking a stale one to an endpoint that never asked.
    return request;
}

} // namespace raikiri::venues::bybit
