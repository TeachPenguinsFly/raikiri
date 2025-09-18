#include <raikiri/net/json_writer.hpp>
#include <raikiri/venues/xt/client_id.hpp>
#include <raikiri/venues/xt/endpoints.hpp>
#include <raikiri/venues/xt/rest.hpp>

#include <charconv>
#include <cstring>
#include <optional>

namespace raikiri::venues::xt {

namespace {

// Header names are XT's, and the prefix is load-bearing: the server rebuilds
// the signed prefix out of the headers it received, so `xt-validate-*` on the
// wire with `validate-*` in the digest is a signature error, not a warning.
constexpr std::string_view k_hdr_algorithms = "validate-algorithms";
constexpr std::string_view k_hdr_appkey = "validate-appkey";
constexpr std::string_view k_hdr_recvwindow = "validate-recvwindow";
constexpr std::string_view k_hdr_timestamp = "validate-timestamp";
constexpr std::string_view k_hdr_signature = "validate-signature";

constexpr std::string_view k_algorithm = "HmacSHA256";
constexpr std::string_view k_recv_window = "5000";

// XT refuses a request whose timestamp is more than a second AHEAD of its own
// clock, and every one of XT's own SDKs backdates rather than risk it — the
// futures SDK goes as far as fetching server time per request. recvWindow
// tolerates 5 s of lateness, so spending half a second of that is free.
constexpr std::uint64_t k_clock_backdate_ms = 500;

// Spot orders are SPOT or LEVER; this adapter models no margin account, and
// LEVER borrows.
constexpr std::string_view k_biz_type = "SPOT";

std::string_view method_name(net::HttpMethod method) noexcept {
    switch (method) {
    case net::HttpMethod::Get:
        return "GET";
    case net::HttpMethod::Post:
        return "POST";
    case net::HttpMethod::Put:
        return "PUT";
    case net::HttpMethod::Delete:
        return "DELETE";
    case net::HttpMethod::Patch:
        return "PATCH";
    }
    return "GET";
}

// Fills the builder's target buffer. A part that would overflow is dropped
// rather than truncated mid-token, so an over-long symbol yields a request XT
// refuses with invalid_symbol instead of one that names a different contract.
class TargetWriter {
public:
    explicit TargetWriter(std::span<char> into) noexcept : into_{into} {}

    void put(std::string_view text) noexcept {
        if (len_ + text.size() > into_.size()) {
            return;
        }
        std::memcpy(into_.data() + len_, text.data(), text.size());
        len_ += text.size();
    }

    void put(std::uint32_t value) noexcept {
        std::array<char, 16> num{};
        if (const auto [stop, ec] = std::to_chars(num.data(), num.data() + num.size(), value);
            ec == std::errc{}) {
            put(std::string_view{num.data(), static_cast<std::size_t>(stop - num.data())});
        }
    }

    [[nodiscard]] net::RestRequest get() const noexcept {
        return {.method = net::HttpMethod::Get,
                .target = std::string_view{into_.data(), len_},
                .body = {}};
    }

private:
    std::span<char> into_;
    std::size_t len_{0};
};

// Appends into a fixed buffer, reporting overflow rather than truncating: a
// truncated signed payload authenticates an order that is not the one intended.
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

    Appender& operator<<(std::int64_t value) noexcept {
        std::array<char, 24> num{};
        const auto [stop, ec] = std::to_chars(num.data(), num.data() + num.size(), value);
        if (ec != std::errc{}) {
            overflow_ = true;
            return *this;
        }
        return *this << std::string_view{num.data(), static_cast<std::size_t>(stop - num.data())};
    }

    [[nodiscard]] bool ok() const noexcept { return !overflow_; }

    [[nodiscard]] std::string_view view() const noexcept {
        return overflow_ ? std::string_view{} : std::string_view{buf_.data(), len_};
    }

private:
    std::span<char> buf_;
    std::size_t len_{0};
    bool overflow_{false};
};

[[nodiscard]] bool all_digits(std::string_view text) noexcept {
    if (text.empty()) {
        return false;
    }
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string_view side_name(Side side) noexcept {
    return side == Side::Buy ? "BUY" : "SELL";
}

[[nodiscard]] std::optional<FixedE9> order_price(const OrderCmd& cmd,
                                                 const InstrumentBase& base) noexcept {
    const auto price = base.ticks_to_price(cmd.price);
    if (price.is_undef() || price.raw <= 0) {
        return std::nullopt;
    }
    return price;
}

[[nodiscard]] std::optional<FixedE9> order_qty(const OrderCmd& cmd,
                                               const InstrumentBase& base) noexcept {
    const auto qty = base.lots_to_qty(cmd.qty);
    if (qty.is_undef() || qty.raw <= 0) {
        return std::nullopt;
    }
    return qty;
}

} // namespace

std::string_view rest_host() noexcept {
    return ep::k_rest_futures;
}

std::string_view spot_rest_host() noexcept {
    return ep::k_rest_spot;
}

std::string_view to_string(BuildError err) noexcept {
    switch (err) {
    case BuildError::UnknownInstrument:
        return "unknown instrument";
    case BuildError::BadQuantity:
        return "bad quantity";
    case BuildError::BadPrice:
        return "bad price";
    case BuildError::FractionalContracts:
        return "xt futures size must be a whole number of contracts";
    case BuildError::BufferTooSmall:
        return "buffer too small";
    case BuildError::NotSigned:
        return "credentials not loaded";
    case BuildError::NoOrderTarget:
        return "xt cancels and amends need the venue order id";
    case BuildError::ClientIdNotEncodable:
        return "client order id is not a ULID";
    case BuildError::MarketBuyNeedsQuoteQty:
        return "xt spot market buy is priced in quote units";
    case BuildError::OptionsUnsupported:
        return "xt options order entry";
    case BuildError::DatedFuturesUnsupported:
        return "xt dated futures order entry";
    case BuildError::SpotPositionsUnsupported:
        return "xt spot position pull";
    }
    return "unknown";
}

std::optional<BuildError> market_refusal(MarketType market) noexcept {
    switch (market) {
    case MarketType::Spot:
    case MarketType::Perp:
        return std::nullopt;
    case MarketType::Future:
        // XT lists dated futures (`btc_usdt_261225`, productType "futures") but
        // the catalogue mapper refuses them and no id for one is ever loaded,
        // so an order naming one is a routing bug rather than a stale table.
        return BuildError::DatedFuturesUnsupported;
    case MarketType::Option:
        return BuildError::OptionsUnsupported;
    }
    return BuildError::OptionsUnsupported;
}

std::string_view position_side(Side side, OrderFlags flags) noexcept {
    // The position a BUY opens is LONG and the one it closes is SHORT. Reading
    // `orderSide` as the position side leaves a closing order opening the
    // opposite side instead — the venue accepts it and the book doubles.
    const bool reduce = has_flag(flags, OrderFlags::ReduceOnly);
    const bool is_long = (side == Side::Buy) != reduce;
    return is_long ? "LONG" : "SHORT";
}

std::string_view time_in_force(TimeInForce tif, OrderFlags flags) noexcept {
    if (has_flag(flags, OrderFlags::PostOnly)) {
        return "GTX";
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

// ------------------------------------------------------------ unsigned ----

net::RestRequest RequestBuilder::server_time() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_futures_server_time, .body = {}};
}

net::RestRequest RequestBuilder::spot_server_time() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_spot_server_time, .body = {}};
}

net::RestRequest RequestBuilder::symbol_list() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_futures_symbols, .body = {}};
}

net::RestRequest RequestBuilder::spot_symbol_list() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_spot_symbols, .body = {}};
}

// Every builder below writes into the same buffer, so a request must be sent
// before the next one is built. One funding poll at a time is the only use.
net::RestRequest RequestBuilder::funding_rate(std::string_view symbol) noexcept {
    TargetWriter writer{target_};
    writer.put(ep::k_futures_funding_rate);
    writer.put("?symbol=");
    writer.put(symbol);
    return writer.get();
}

net::RestRequest RequestBuilder::depth(std::string_view symbol, std::uint32_t level) noexcept {
    TargetWriter writer{target_};
    writer.put(ep::k_futures_depth);
    writer.put("?symbol=");
    writer.put(symbol);
    writer.put("&level=");
    writer.put(level);
    return writer.get();
}

// -------------------------------------------------------------- signed ----

std::string_view SignedRequestBuilder::last_sign_message() const noexcept {
    return {sign_msg_.data(), sign_msg_len_};
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::sign_spot(net::HttpMethod method,
                                                                            std::string_view path,
                                                                            std::string_view query,
                                                                            std::string_view body,
                                                                            UnixNanos now) {
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NotSigned};
    }
    Appender ts{timestamp_};
    ts << static_cast<std::int64_t>(now.ns / 1'000'000ULL - k_clock_backdate_ms);
    if (!ts.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }

    // X is the four headers as `k=v` SORTED BY NAME; the order below is that
    // sort, and re-ordering it silently invalidates every signature.
    Appender msg{sign_msg_};
    msg << k_hdr_algorithms << "=" << k_algorithm << "&" << k_hdr_appkey << "="
        << signer_->api_key() << "&" << k_hdr_recvwindow << "=" << k_recv_window << "&"
        << k_hdr_timestamp << "=" << ts.view();
    // Y appends only the components that are PRESENT — an empty query does not
    // become an empty segment, it disappears.
    msg << "#" << method_name(method) << "#" << path;
    if (!query.empty()) {
        msg << "#" << query;
    }
    if (!body.empty()) {
        msg << "#" << body;
    }
    if (!msg.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    sign_msg_len_ = msg.view().size();

    const auto signature = signer_->sign(msg.view(), net::Signer::Encoding::Hex, signature_);
    if (signature.empty()) {
        return std::unexpected{BuildError::NotSigned};
    }

    Appender target{target_};
    target << path;
    if (!query.empty()) {
        target << "?" << query;
    }
    if (!target.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }

    net::RestRequest request{};
    request.method = method;
    request.target = target.view();
    request.body = body;
    request.headers = {{k_hdr_algorithms, k_algorithm},
                       {k_hdr_appkey, signer_->api_key()},
                       {k_hdr_recvwindow, k_recv_window},
                       {k_hdr_timestamp, ts.view()},
                       {k_hdr_signature, signature}};
    return request;
}

std::expected<net::RestRequest, BuildError>
SignedRequestBuilder::sign_futures(net::HttpMethod method,
                                   std::string_view path,
                                   std::string_view query,
                                   std::string_view body,
                                   UnixNanos now) {
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NotSigned};
    }
    Appender ts{timestamp_};
    ts << static_cast<std::int64_t>(now.ns / 1'000'000ULL - k_clock_backdate_ms);
    if (!ts.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }

    // Two fields, not four, and NO METHOD in Y. Signing the spot way here
    // produces a digest the futures host rejects as a bad key.
    Appender msg{sign_msg_};
    msg << k_hdr_appkey << "=" << signer_->api_key() << "&" << k_hdr_timestamp << "=" << ts.view();
    msg << "#" << path;
    // One parameter slot: a GET's query or a POST's body, never both.
    if (!query.empty()) {
        msg << "#" << query;
    } else if (!body.empty()) {
        msg << "#" << body;
    }
    if (!msg.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    sign_msg_len_ = msg.view().size();

    const auto signature = signer_->sign(msg.view(), net::Signer::Encoding::Hex, signature_);
    if (signature.empty()) {
        return std::unexpected{BuildError::NotSigned};
    }

    Appender target{target_};
    target << path;
    if (!query.empty()) {
        target << "?" << query;
    }
    if (!target.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }

    net::RestRequest request{};
    request.method = method;
    request.target = target.view();
    request.body = body;
    request.headers = {{k_hdr_appkey, signer_->api_key()},
                       {k_hdr_timestamp, ts.view()},
                       {k_hdr_signature, signature}};
    return request;
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::create_order(
    const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
    if (const auto refusal = market_refusal(cmd.instrument.market()); refusal.has_value()) {
        return std::unexpected{*refusal};
    }
    const auto* instrument = table.find(cmd.instrument);
    if (instrument == nullptr) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    const auto& base = base_of(*instrument);
    const auto symbol = base.exchange_symbol.view();
    const auto coid = encode_client_id(cmd.exchange_client_order_id);
    if (!coid.has_value()) {
        return std::unexpected{BuildError::ClientIdNotEncodable};
    }
    const auto qty = order_qty(cmd, base);
    if (!qty.has_value()) {
        return std::unexpected{BuildError::BadQuantity};
    }
    const bool is_market = cmd.order_type == OrderType::Market;

    switch (cmd.instrument.market()) {
    case MarketType::Perp: {
        // origQty counts CONTRACTS and XT answers invalid_quantity_scale to a
        // fractional one, so a size that does not land on a whole contract is
        // refused rather than rounded to a position of a different size.
        if (qty->raw % k_e9 != 0) {
            return std::unexpected{BuildError::FractionalContracts};
        }
        net::JsonWriter json{body_};
        json.obj_begin()
            .str("symbol", symbol)
            .str("orderSide", side_name(cmd.side))
            .str("orderType", is_market ? "MARKET" : "LIMIT")
            .num("origQty", qty->raw / k_e9);
        if (!is_market) {
            const auto price = order_price(cmd, base);
            if (!price.has_value()) {
                return std::unexpected{BuildError::BadPrice};
            }
            // `price` is declared a JSON NUMBER on this tree; quoting it is an
            // invalid_params refusal.
            json.decimal_raw("price", *price);
        }
        json.str("timeInForce", time_in_force(cmd.tif, cmd.flags))
            .str("positionSide", position_side(cmd.side, cmd.flags))
            .str("clientOrderId", coid->view())
            .obj_end();
        if (!json.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return sign_futures(
            net::HttpMethod::Post, ep::k_futures_order_create, {}, json.view(), now);
    }
    case MarketType::Spot: {
        if (is_market && cmd.side == Side::Buy) {
            // A market BUY is sized in QUOTE units here (`quoteQty`) and XT
            // refuses `quantity` on one. OrderCmd carries a base size, so the
            // conversion would need a price this builder would have to invent.
            return std::unexpected{BuildError::MarketBuyNeedsQuoteQty};
        }
        net::JsonWriter json{body_};
        json.obj_begin()
            .str("symbol", symbol)
            .str("clientOrderId", coid->view())
            .str("side", side_name(cmd.side))
            .str("type", is_market ? "MARKET" : "LIMIT")
            .str("timeInForce", time_in_force(cmd.tif, cmd.flags))
            .str("bizType", k_biz_type);
        if (!is_market) {
            const auto price = order_price(cmd, base);
            if (!price.has_value()) {
                return std::unexpected{BuildError::BadPrice};
            }
            json.decimal_raw("price", *price);
        }
        // ReduceOnly is deliberately not forwarded: a spot balance cannot be
        // short, so XT has no field for it.
        json.decimal_raw("quantity", *qty).obj_end();
        if (!json.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return sign_spot(net::HttpMethod::Post, ep::k_spot_order, {}, json.view(), now);
    }
    case MarketType::Future:
        return std::unexpected{BuildError::DatedFuturesUnsupported};
    case MarketType::Option:
        return std::unexpected{BuildError::OptionsUnsupported};
    }
    return std::unexpected{BuildError::UnknownInstrument};
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::cancel_order(
    const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
    if (const auto refusal = market_refusal(cmd.instrument.market()); refusal.has_value()) {
        return std::unexpected{*refusal};
    }
    if (table.find(cmd.instrument) == nullptr) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    const auto venue_id = cmd.target_exchange_order_id.view();
    if (!all_digits(venue_id)) {
        return std::unexpected{BuildError::NoOrderTarget};
    }

    switch (cmd.instrument.market()) {
    case MarketType::Perp: {
        net::JsonWriter json{body_};
        // `orderId` is declared an integer, and XT's own ids are digit runs,
        // so it goes in UNQUOTED.
        json.obj_begin().raw("orderId", venue_id).obj_end();
        if (!json.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return sign_futures(
            net::HttpMethod::Post, ep::k_futures_order_cancel, {}, json.view(), now);
    }
    case MarketType::Spot: {
        // The order id is a PATH SEGMENT here, so it is inside the signature's
        // path term rather than beside it. The body is empty.
        Appender path{path_};
        path << ep::k_spot_order << "/" << venue_id;
        if (!path.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return sign_spot(net::HttpMethod::Delete, path.view(), {}, {}, now);
    }
    case MarketType::Future:
        return std::unexpected{BuildError::DatedFuturesUnsupported};
    case MarketType::Option:
        return std::unexpected{BuildError::OptionsUnsupported};
    }
    return std::unexpected{BuildError::UnknownInstrument};
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::amend_order(
    const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
    if (const auto refusal = market_refusal(cmd.instrument.market()); refusal.has_value()) {
        return std::unexpected{*refusal};
    }
    const auto* instrument = table.find(cmd.instrument);
    if (instrument == nullptr) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    const auto& base = base_of(*instrument);
    const auto venue_id = cmd.target_exchange_order_id.view();
    if (!all_digits(venue_id)) {
        return std::unexpected{BuildError::NoOrderTarget};
    }
    // Both amend endpoints take price AND quantity together, so an amend that
    // moves one still has to restate the other; a zero in either field would
    // be read as the new value.
    const auto price = order_price(cmd, base);
    const auto qty = order_qty(cmd, base);
    if (!price.has_value()) {
        return std::unexpected{BuildError::BadPrice};
    }
    if (!qty.has_value()) {
        return std::unexpected{BuildError::BadQuantity};
    }

    switch (cmd.instrument.market()) {
    case MarketType::Perp: {
        if (qty->raw % k_e9 != 0) {
            return std::unexpected{BuildError::FractionalContracts};
        }
        net::JsonWriter json{body_};
        json.obj_begin()
            .raw("orderId", venue_id)
            .decimal_raw("price", *price)
            .num("origQty", qty->raw / k_e9)
            .obj_end();
        if (!json.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return sign_futures(
            net::HttpMethod::Post, ep::k_futures_order_update, {}, json.view(), now);
    }
    case MarketType::Spot: {
        Appender path{path_};
        path << ep::k_spot_order << "/" << venue_id;
        if (!path.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        net::JsonWriter json{body_};
        json.obj_begin()
            .raw("orderId", venue_id)
            .decimal_raw("price", *price)
            .decimal_raw("quantity", *qty)
            .obj_end();
        if (!json.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return sign_spot(net::HttpMethod::Put, path.view(), {}, json.view(), now);
    }
    case MarketType::Future:
        return std::unexpected{BuildError::DatedFuturesUnsupported};
    case MarketType::Option:
        return std::unexpected{BuildError::OptionsUnsupported};
    }
    return std::unexpected{BuildError::UnknownInstrument};
}

std::expected<net::RestRequest, BuildError>
SignedRequestBuilder::open_orders(MarketType market, std::string_view symbol, UnixNanos now) {
    switch (market) {
    case MarketType::Perp: {
        if (symbol.empty()) {
            return sign_futures(net::HttpMethod::Post, ep::k_futures_open_orders, {}, {}, now);
        }
        net::JsonWriter json{body_};
        json.obj_begin().str("symbol", symbol).obj_end();
        if (!json.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return sign_futures(net::HttpMethod::Post, ep::k_futures_open_orders, {}, json.view(), now);
    }
    case MarketType::Spot: {
        // Query parameters go into the digest SORTED BY NAME, which is what
        // the order below is; `bizType` precedes `symbol`.
        Appender query{query_};
        query << "bizType=" << k_biz_type;
        if (!symbol.empty()) {
            query << "&symbol=" << symbol;
        }
        if (!query.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return sign_spot(net::HttpMethod::Get, ep::k_spot_open_orders, query.view(), {}, now);
    }
    case MarketType::Future:
        return std::unexpected{BuildError::DatedFuturesUnsupported};
    case MarketType::Option:
        return std::unexpected{BuildError::OptionsUnsupported};
    }
    return std::unexpected{BuildError::UnknownInstrument};
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::query_order(
    const ExchangeClientOrderId& id, MarketType market, UnixNanos now) {
    const auto coid = encode_client_id(id);
    if (!coid.has_value()) {
        return std::unexpected{BuildError::ClientIdNotEncodable};
    }
    switch (market) {
    case MarketType::Spot: {
        Appender query{query_};
        query << "clientOrderId=" << coid->view();
        if (!query.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return sign_spot(net::HttpMethod::Get, ep::k_spot_order, query.view(), {}, now);
    }
    case MarketType::Perp:
        // /future/trade/v1/order/detail is keyed by the VENUE order id and XT
        // publishes no by-client-id read, so this question is answered from the
        // open-order sweep instead of by an endpoint that cannot hear it.
        return std::unexpected{BuildError::NoOrderTarget};
    case MarketType::Future:
        return std::unexpected{BuildError::DatedFuturesUnsupported};
    case MarketType::Option:
        return std::unexpected{BuildError::OptionsUnsupported};
    }
    return std::unexpected{BuildError::UnknownInstrument};
}

std::expected<net::RestRequest, BuildError>
SignedRequestBuilder::my_trades(MarketType market, UnixNanos since, UnixNanos now) {
    const auto since_ms = static_cast<std::int64_t>(since.ns / 1'000'000ULL);
    switch (market) {
    case MarketType::Perp: {
        Appender query{query_};
        query << "page=1&size=" << static_cast<std::int64_t>(k_page_size);
        if (since_ms > 0) {
            query << "&startTime=" << since_ms;
        }
        if (!query.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return sign_futures(net::HttpMethod::Get, ep::k_futures_trades, query.view(), {}, now);
    }
    case MarketType::Spot: {
        Appender query{query_};
        query << "bizType=" << k_biz_type << "&limit=" << static_cast<std::int64_t>(k_page_size);
        if (since_ms > 0) {
            query << "&startTime=" << since_ms;
        }
        if (!query.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return sign_spot(net::HttpMethod::Get, ep::k_spot_trades, query.view(), {}, now);
    }
    case MarketType::Future:
        return std::unexpected{BuildError::DatedFuturesUnsupported};
    case MarketType::Option:
        return std::unexpected{BuildError::OptionsUnsupported};
    }
    return std::unexpected{BuildError::UnknownInstrument};
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::positions(MarketType market,
                                                                            UnixNanos now) {
    switch (market) {
    case MarketType::Perp:
        return sign_futures(net::HttpMethod::Get, ep::k_futures_positions, {}, {}, now);
    case MarketType::Spot:
        // A spot balance is not a position. Answering "flat" would be a claim
        // about a concept this API does not have.
        return std::unexpected{BuildError::SpotPositionsUnsupported};
    case MarketType::Future:
        return std::unexpected{BuildError::DatedFuturesUnsupported};
    case MarketType::Option:
        return std::unexpected{BuildError::OptionsUnsupported};
    }
    return std::unexpected{BuildError::UnknownInstrument};
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::futures_balances(UnixNanos now) {
    return sign_futures(net::HttpMethod::Get, ep::k_futures_balances, {}, {}, now);
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::bills(UnixNanos since,
                                                                        UnixNanos now) {
    Appender query{body_};
    // Newest first, one page. A ledger this process has fallen far behind on
    // is a reconciliation problem and not something to paper over by walking
    // pages inside a call the poller makes on a timer.
    query << "direction=NEXT&limit=100";
    if (since.ns != 0) {
        query << "&startTime=" << static_cast<std::int64_t>(since.ns / 1'000'000ULL);
    }
    if (!query.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign_futures(net::HttpMethod::Get, ep::k_futures_bills, query.view(), {}, now);
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::listen_key(UnixNanos now) {
    return sign_futures(net::HttpMethod::Get, ep::k_futures_listen_key, {}, {}, now);
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::ws_token(UnixNanos now) {
    return sign_spot(net::HttpMethod::Post, ep::k_spot_ws_token, {}, {}, now);
}

} // namespace raikiri::venues::xt
