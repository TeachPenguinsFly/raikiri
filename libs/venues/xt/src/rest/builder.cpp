#include <raikiri/net/json_writer.hpp>
#include <raikiri/venues/xt/client_id.hpp>
#include <raikiri/venues/xt/endpoints.hpp>
#include <raikiri/venues/xt/rest.hpp>

#include <charconv>
#include <cstring>
#include <optional>

namespace raikiri::venues::xt {

namespace {

// the header prefix sent must match the signed one; the server rebuilds the signed string from received headers
constexpr std::string_view k_hdr_algorithms = "validate-algorithms";
constexpr std::string_view k_hdr_appkey = "validate-appkey";
constexpr std::string_view k_hdr_recvwindow = "validate-recvwindow";
constexpr std::string_view k_hdr_timestamp = "validate-timestamp";
constexpr std::string_view k_hdr_signature = "validate-signature";

constexpr std::string_view k_algorithm = "HmacSHA256";
constexpr std::string_view k_recv_window = "5000";

// xt refuses timestamps more than 1 s ahead and recvWindow allows 5 s late, so backdate
constexpr std::uint64_t k_clock_backdate_ms = 500;

// LEVER borrows; no margin account is modelled
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

// an overflowing part is dropped whole, so xt refuses it as invalid_symbol
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
        if (const auto [stop, ec] = std::to_chars(num.data(), num.data() + num.size(), value); ec == std::errc{}) {
            put(std::string_view{num.data(), static_cast<std::size_t>(stop - num.data())});
        }
    }

    [[nodiscard]] net::RestRequest get() const noexcept {
        return {.method = net::HttpMethod::Get, .target = std::string_view{into_.data(), len_}, .body = {}};
    }

private:
    std::span<char> into_;
    std::size_t len_{0};
};

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

[[nodiscard]] std::optional<FixedE9> order_price(const OrderCmd& cmd, const InstrumentBase& base) noexcept {
    const auto price = base.ticks_to_price(cmd.price);
    if (price.is_undef() || price.raw <= 0) {
        return std::nullopt;
    }
    return price;
}

[[nodiscard]] std::optional<FixedE9> order_qty(const OrderCmd& cmd, const InstrumentBase& base) noexcept {
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
        // dated contracts are never loaded, so this is a routing bug
        return BuildError::DatedFuturesUnsupported;
    case MarketType::Option:
        return BuildError::OptionsUnsupported;
    }
    return BuildError::OptionsUnsupported;
}

std::string_view position_side(Side side, OrderFlags flags) noexcept {
    // a buy opens LONG and closes SHORT
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

// shares one buffer; send before building the next
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

std::string_view SignedRequestBuilder::last_sign_message() const noexcept {
    return {sign_msg_.data(), sign_msg_len_};
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::sign_spot(
    net::HttpMethod method, std::string_view path, std::string_view query, std::string_view body, UnixNanos now) {
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NotSigned};
    }
    Appender ts{timestamp_};
    ts << static_cast<std::int64_t>(now.ns / 1'000'000ULL - k_clock_backdate_ms);
    if (!ts.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }

    // X is the four headers sorted by name; keep this order
    Appender msg{sign_msg_};
    msg << k_hdr_algorithms << "=" << k_algorithm << "&" << k_hdr_appkey << "=" << signer_->api_key() << "&"
        << k_hdr_recvwindow << "=" << k_recv_window << "&" << k_hdr_timestamp << "=" << ts.view();
    // Y appends only present components; an empty query adds no segment
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

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::sign_futures(
    net::HttpMethod method, std::string_view path, std::string_view query, std::string_view body, UnixNanos now) {
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NotSigned};
    }
    Appender ts{timestamp_};
    ts << static_cast<std::int64_t>(now.ns / 1'000'000ULL - k_clock_backdate_ms);
    if (!ts.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }

    // futures: two headers and no method in Y
    Appender msg{sign_msg_};
    msg << k_hdr_appkey << "=" << signer_->api_key() << "&" << k_hdr_timestamp << "=" << ts.view();
    msg << "#" << path;
    // one parameter slot: a GET's query or a POST's body
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
    request.headers = {{k_hdr_appkey, signer_->api_key()}, {k_hdr_timestamp, ts.view()}, {k_hdr_signature, signature}};
    return request;
}

std::expected<net::RestRequest, BuildError>
SignedRequestBuilder::create_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
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
        // origQty is whole contracts; xt refuses fractions with invalid_quantity_scale
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
            // price must be a json number here; quoted is refused as invalid_params
            json.decimal_raw("price", *price);
        }
        json.str("timeInForce", time_in_force(cmd.tif, cmd.flags))
            .str("positionSide", position_side(cmd.side, cmd.flags))
            .str("clientOrderId", coid->view())
            .obj_end();
        if (!json.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return sign_futures(net::HttpMethod::Post, ep::k_futures_order_create, {}, json.view(), now);
    }
    case MarketType::Spot: {
        if (is_market && cmd.side == Side::Buy) {
            // a spot market buy needs quoteQty and OrderCmd carries a base size
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
        // spot has no reduce-only field
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

std::expected<net::RestRequest, BuildError>
SignedRequestBuilder::cancel_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
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
        // orderId is declared an integer, so it is sent unquoted
        json.obj_begin().raw("orderId", venue_id).obj_end();
        if (!json.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return sign_futures(net::HttpMethod::Post, ep::k_futures_order_cancel, {}, json.view(), now);
    }
    case MarketType::Spot: {
        // the order id is a path segment and so part of the signed path; empty body
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

std::expected<net::RestRequest, BuildError>
SignedRequestBuilder::amend_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
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
    // both amends take price and quantity together; a zero would be read as the new value
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
        return sign_futures(net::HttpMethod::Post, ep::k_futures_order_update, {}, json.view(), now);
    }
    case MarketType::Spot: {
        Appender path{path_};
        path << ep::k_spot_order << "/" << venue_id;
        if (!path.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        net::JsonWriter json{body_};
        json.obj_begin().raw("orderId", venue_id).decimal_raw("price", *price).decimal_raw("quantity", *qty).obj_end();
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
        // query params are signed sorted by name; keep bizType before symbol
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

std::expected<net::RestRequest, BuildError>
SignedRequestBuilder::query_order(const ExchangeClientOrderId& id, MarketType market, UnixNanos now) {
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

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::positions(MarketType market, UnixNanos now) {
    switch (market) {
    case MarketType::Perp:
        return sign_futures(net::HttpMethod::Get, ep::k_futures_positions, {}, {}, now);
    case MarketType::Spot:
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

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::bills(UnixNanos since, UnixNanos now) {
    Appender query{body_};
    // one page, newest first; falling far behind is for reconciliation
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
