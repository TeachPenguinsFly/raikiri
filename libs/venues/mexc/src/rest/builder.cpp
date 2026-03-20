#include <raikiri/net/json_writer.hpp>
#include <raikiri/venues/mexc/endpoints.hpp>
#include <raikiri/venues/mexc/rest.hpp>

#include <charconv>
#include <cstring>
#include <optional>
#include <span>

namespace raikiri::venues::mexc {

namespace {

// Header names are MEXC's, capitalisation and all.
constexpr std::string_view k_hdr_spot_key = "X-MEXC-APIKEY";
constexpr std::string_view k_hdr_fut_key = "ApiKey";
constexpr std::string_view k_hdr_fut_time = "Request-Time";
constexpr std::string_view k_hdr_fut_sign = "Signature";
constexpr std::string_view k_hdr_fut_recv = "Recv-Window";

// Spot v3 takes its parameters in the query string even on a POST, and the
// body stays empty; the content type still has to be the form one, because
// application/json on an empty body is answered as a parameter error.
constexpr std::string_view k_form = "application/x-www-form-urlencoded";

// MEXC refuses a signature whose timestamp is outside recvWindow of ITS clock.
// Five seconds is the venue default and 60 s the ceiling.
constexpr std::uint32_t k_recv_window_ms = 5'000;

// Both list endpoints page; reconciliation wants the whole picture in one
// round trip rather than a paginated approximation of it.
constexpr std::string_view k_page_size = "100";

// MEXC has no margin-mode field in VenueConfig, and openType is mandatory on
// every futures order. Cross is the one of the two that does not ALSO demand a
// leverage value — isolated (1) is refused without one, and inventing a
// leverage here would be a position-sizing decision made by a request builder.
constexpr std::string_view k_open_type_cross = "2";

// Fills the builder's target buffer. A part that would overflow is dropped
// rather than truncated mid-token, so an over-long symbol yields a request
// MEXC refuses with 1001 instead of one that names a different contract.
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
// truncated signed query authenticates an order that is not the one intended.
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

    Appender& operator<<(FixedE9 value) noexcept {
        std::array<char, 40> buf{};
        const auto text = format_e9(value, buf.data(), buf.size());
        if (text.empty()) {
            overflow_ = true;
            return *this;
        }
        return *this << text;
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

// MEXC's futures order ids are declared List<Long> where a cancel names them,
// so a non-numeric id cannot go into that array at all.
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

[[nodiscard]] std::string_view spot_side(Side side) noexcept {
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
    return ep::k_rest_public;
}

std::string_view private_rest_host() noexcept {
    return ep::k_rest_private;
}

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
    case BuildError::NoOrderTarget:
        return "no order id and no client order id";
    case BuildError::SpotTifNotExpressible:
        return "mexc spot has no time in force";
    case BuildError::AmendUnsupported:
        return "mexc order amend";
    case BuildError::OptionsUnsupported:
        return "mexc options order entry";
    case BuildError::DatedFuturesUnsupported:
        return "mexc dated futures order entry";
    case BuildError::NeedsSymbol:
        return "mexc spot reads need a symbol";
    case BuildError::SpotBatchCancelUnsupported:
        return "mexc spot batch cancel by id";
    case BuildError::SpotPositionsUnsupported:
        return "mexc spot position pull";
    case BuildError::SpotFundingUnsupported:
        return "mexc spot funding charges";
    }
    return "unknown";
}

std::optional<BuildError> market_refusal(MarketType market) noexcept {
    switch (market) {
    case MarketType::Spot:
    case MarketType::Perp:
        return std::nullopt;
    case MarketType::Future:
        // /api/v1/contract serves perpetuals only; MEXC lists no dated futures.
        return BuildError::DatedFuturesUnsupported;
    case MarketType::Option:
        return BuildError::OptionsUnsupported;
    }
    return BuildError::OptionsUnsupported;
}

std::int64_t futures_side(Side side, OrderFlags flags) noexcept {
    const bool reduce = has_flag(flags, OrderFlags::ReduceOnly);
    if (side == Side::Buy) {
        return reduce ? 2 : 1; // close short : open long
    }
    return reduce ? 4 : 3; // close long : open short
}

std::int64_t futures_order_type(OrderType type, TimeInForce tif, OrderFlags flags) noexcept {
    if (type == OrderType::Market) {
        return 5;
    }
    if (has_flag(flags, OrderFlags::PostOnly)) {
        return 2; // post-only is an order TYPE here, not a flag beside the tif
    }
    switch (tif) {
    case TimeInForce::Ioc:
        return 3;
    case TimeInForce::Fok:
        return 4;
    case TimeInForce::Gtc:
        break;
    }
    return 1;
}

std::string_view spot_order_type(OrderType type, OrderFlags flags) noexcept {
    if (type == OrderType::Market) {
        return "MARKET";
    }
    return has_flag(flags, OrderFlags::PostOnly) ? "LIMIT_MAKER" : "LIMIT";
}

// ------------------------------------------------------------ unsigned ----

net::RestRequest RequestBuilder::server_time() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_futures_server_time, .body = {}};
}

net::RestRequest RequestBuilder::spot_server_time() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_spot_server_time, .body = {}};
}

net::RestRequest RequestBuilder::contract_detail() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_contract_detail, .body = {}};
}

net::RestRequest RequestBuilder::exchange_info() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_exchange_info, .body = {}};
}

// Every builder below writes into the same buffer, so a request must be sent
// before the next one is built. One catalogue load at a time is the only use.
net::RestRequest RequestBuilder::depth(std::string_view symbol, std::uint32_t limit) noexcept {
    TargetWriter writer{target_};
    writer.put(ep::k_contract_depth);
    writer.put("/");
    writer.put(symbol);
    if (limit != 0) {
        writer.put("?limit=");
        writer.put(limit);
    }
    return writer.get();
}

net::RestRequest RequestBuilder::ticker(std::string_view symbol) noexcept {
    TargetWriter writer{target_};
    writer.put(ep::k_contract_ticker);
    writer.put("?symbol=");
    writer.put(symbol);
    return writer.get();
}

net::RestRequest RequestBuilder::funding_rates() noexcept {
    TargetWriter writer{target_};
    writer.put(ep::k_contract_funding_rate);
    return writer.get();
}

net::RestRequest RequestBuilder::funding_rate(std::string_view symbol) noexcept {
    TargetWriter writer{target_};
    writer.put(ep::k_contract_funding_rate);
    writer.put("/");
    writer.put(symbol);
    return writer.get();
}

// -------------------------------------------------------------- signed ----

std::string_view SignedRequestBuilder::last_sign_message() const noexcept {
    return {sign_msg_.data(), sign_msg_len_};
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::sign_spot(net::HttpMethod method,
                                                                            std::string_view path,
                                                                            std::string_view query,
                                                                            UnixNanos now) {
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NotSigned};
    }
    // The digest covers the query EXACTLY as it goes out, so `signature` is
    // appended after signing and nothing may re-order what precedes it.
    Appender msg{sign_msg_};
    msg << query;
    if (!query.empty()) {
        msg << "&";
    }
    msg << "recvWindow=" << static_cast<std::int64_t>(k_recv_window_ms)
        << "&timestamp=" << static_cast<std::int64_t>(now.ns / 1'000'000ULL);
    if (!msg.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    sign_msg_len_ = msg.view().size();

    const auto signature = signer_->sign(msg.view(), net::Signer::Encoding::Hex, signature_);
    if (signature.empty()) {
        return std::unexpected{BuildError::NotSigned};
    }

    Appender target{target_};
    target << path << "?" << msg.view() << "&signature=" << signature;
    if (!target.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }

    net::RestRequest request{};
    request.method = method;
    request.target = target.view();
    request.body = {};
    request.content_type = k_form;
    request.headers = {{k_hdr_spot_key, signer_->api_key()}};
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
    ts << static_cast<std::int64_t>(now.ns / 1'000'000ULL);
    if (!ts.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }

    // apiKey + Request-Time + (sorted query | raw JSON body) — the parameter
    // term is one or the other, never both, and the separator is nothing at
    // all. A GET carries its parameters in the query and a POST in the body,
    // so a POST that signed its query would authenticate an empty order.
    Appender msg{sign_msg_};
    msg << signer_->api_key() << ts.view() << (body.empty() ? query : body);
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
    request.headers = {{k_hdr_fut_key, signer_->api_key()},
                       {k_hdr_fut_time, ts.view()},
                       {k_hdr_fut_recv, "60"},
                       {k_hdr_fut_sign, signature}};
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
    const auto coid = cmd.exchange_client_order_id.view();
    if (coid.empty() || coid.size() > k_client_id_max) {
        return std::unexpected{BuildError::NoOrderTarget};
    }
    const auto qty = order_qty(cmd, base);
    if (!qty.has_value()) {
        return std::unexpected{BuildError::BadQuantity};
    }
    const bool is_market = cmd.order_type == OrderType::Market;

    switch (cmd.instrument.market()) {
    case MarketType::Perp: {
        net::JsonWriter json{body_};
        json.obj_begin().str("symbol", symbol);
        if (!is_market) {
            const auto price = order_price(cmd, base);
            if (!price.has_value()) {
                return std::unexpected{BuildError::BadPrice};
            }
            // `price` is a JSON NUMBER on this API; quoting it is answered 600.
            json.decimal_raw("price", *price);
        }
        // `vol` counts CONTRACTS, and the instrument's lot step is already
        // stated in them, so this is the lot count rendered as a decimal.
        json.decimal_raw("vol", *qty)
            .num("side", futures_side(cmd.side, cmd.flags))
            .num("type", futures_order_type(cmd.order_type, cmd.tif, cmd.flags))
            .raw("openType", k_open_type_cross)
            .str("externalOid", coid);
        if (has_flag(cmd.flags, OrderFlags::ReduceOnly)) {
            // `side` already says close in hedge mode; this is what says it in
            // one-way mode, and MEXC takes whichever its account setting uses.
            json.boolean("reduceOnly", true);
        }
        json.obj_end();
        if (!json.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return sign_futures(
            net::HttpMethod::Post, ep::k_futures_order_create, {}, json.view(), now);
    }
    case MarketType::Spot: {
        // MEXC spot advertises no timeInForce parameter at all: IOC and FOK
        // have nowhere to go, and silently sending GTC would leave a resting
        // order where the strategy asked for a fill-or-nothing.
        if (cmd.tif != TimeInForce::Gtc) {
            return std::unexpected{BuildError::SpotTifNotExpressible};
        }
        Appender query{query_};
        query << "symbol=" << symbol << "&side=" << spot_side(cmd.side)
              << "&type=" << spot_order_type(cmd.order_type, cmd.flags) << "&quantity=" << *qty;
        if (!is_market) {
            const auto price = order_price(cmd, base);
            if (!price.has_value()) {
                return std::unexpected{BuildError::BadPrice};
            }
            query << "&price=" << *price;
        }
        // ReduceOnly is deliberately not forwarded: a spot balance cannot be
        // short, so MEXC has no field for it.
        query << "&newClientOrderId=" << coid;
        if (!query.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return sign_spot(net::HttpMethod::Post, ep::k_spot_order, query.view(), now);
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
    const auto symbol = table.exchange_symbol(cmd.instrument);
    if (symbol.empty()) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    const auto venue_id = cmd.target_exchange_order_id.view();
    const auto coid = cmd.exchange_client_order_id.view();
    if (venue_id.empty() && coid.empty()) {
        return std::unexpected{BuildError::NoOrderTarget};
    }

    switch (cmd.instrument.market()) {
    case MarketType::Perp: {
        if (venue_id.empty()) {
            // cancel_with_external takes ONE order and names it by symbol plus
            // externalOid; the id-array endpoint cannot express a client id.
            net::JsonWriter json{body_};
            json.obj_begin().str("symbol", symbol).str("externalOid", coid).obj_end();
            if (!json.ok()) {
                return std::unexpected{BuildError::BufferTooSmall};
            }
            return sign_futures(net::HttpMethod::Post,
                                ep::k_futures_order_cancel_by_client_id,
                                {},
                                json.view(),
                                now);
        }
        // The body IS a bare array of ids — not an object wrapping one — and
        // the ids are declared List<Long>, so they go in UNQUOTED. An id that
        // is not a run of digits cannot be rendered that way and is refused
        // rather than emitted as JSON MEXC would answer 601 to.
        if (!all_digits(venue_id)) {
            return std::unexpected{BuildError::NoOrderTarget};
        }
        Appender body{body_};
        body << "[" << venue_id << "]";
        if (!body.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return sign_futures(
            net::HttpMethod::Post, ep::k_futures_order_cancel, {}, body.view(), now);
    }
    case MarketType::Spot: {
        Appender query{query_};
        query << "symbol=" << symbol;
        if (!venue_id.empty()) {
            query << "&orderId=" << venue_id;
        } else {
            query << "&origClientOrderId=" << coid;
        }
        if (!query.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return sign_spot(net::HttpMethod::Delete, ep::k_spot_order, query.view(), now);
    }
    case MarketType::Future:
        return std::unexpected{BuildError::DatedFuturesUnsupported};
    case MarketType::Option:
        return std::unexpected{BuildError::OptionsUnsupported};
    }
    return std::unexpected{BuildError::UnknownInstrument};
}

std::expected<net::RestRequest, BuildError>
SignedRequestBuilder::amend_order(const OrderCmd& cmd, const exec::InstrumentTable&, UnixNanos) {
    if (const auto refusal = market_refusal(cmd.instrument.market()); refusal.has_value()) {
        return std::unexpected{*refusal};
    }
    switch (cmd.instrument.market()) {
    case MarketType::Spot:
    case MarketType::Perp:
    case MarketType::Future:
        // Neither MEXC API has a modify endpoint. Synthesizing cancel-replace
        // here would report an Updated the venue never granted, and would hide
        // the queue position the order actually lost.
        return std::unexpected{BuildError::AmendUnsupported};
    case MarketType::Option:
        return std::unexpected{BuildError::OptionsUnsupported};
    }
    return std::unexpected{BuildError::AmendUnsupported};
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::cancel_batch(
    std::span<const OrderCmd> cmds, const exec::InstrumentTable& table, UnixNanos now) {
    if (cmds.empty() || cmds.size() > k_max_batch_cancel) {
        return std::unexpected{BuildError::NoOrderTarget};
    }
    const auto market = cmds.front().instrument.market();
    if (const auto refusal = market_refusal(market); refusal.has_value()) {
        return std::unexpected{*refusal};
    }
    // A batch that straddles markets would go to one endpoint and silently
    // lose the other market's cancels.
    for (const auto& cmd : cmds) {
        if (cmd.instrument.market() != market) {
            return std::unexpected{BuildError::UnknownInstrument};
        }
    }

    switch (market) {
    case MarketType::Perp: {
        Appender body{body_};
        body << "[";
        for (std::size_t i = 0; i < cmds.size(); ++i) {
            // The array endpoint speaks venue ids only; an order we know only
            // by our own id goes one at a time through cancel_with_external.
            const auto venue_id = cmds[i].target_exchange_order_id.view();
            if (!all_digits(venue_id)) {
                return std::unexpected{BuildError::NoOrderTarget};
            }
            body << (i == 0 ? "" : ",") << venue_id;
        }
        body << "]";
        if (!body.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return sign_futures(
            net::HttpMethod::Post, ep::k_futures_order_cancel, {}, body.view(), now);
    }
    case MarketType::Spot:
        // MEXC spot cancels by symbol or by single order, never by a list of
        // ids. Refused so the caller loops instead of cancelling a symbol's
        // whole book because a batch was asked for.
        (void)table;
        return std::unexpected{BuildError::SpotBatchCancelUnsupported};
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
        // The path buffer is a member, not a local: sign_futures copies the
        // path into `target_` and the request that leaves here still points at
        // the bytes it was given.
        Appender path{path_};
        // The symbol is a PATH segment, and the two doc generations disagree
        // about it: the legacy tree spells the sweep .../open_orders/{symbol}
        // while the current one dropped the segment entirely. Naming a symbol
        // keeps the old form and leaving it out keeps the new one, so a
        // trailing empty segment is never emitted.
        path << ep::k_futures_open_orders;
        if (!symbol.empty()) {
            path << "/" << symbol;
        }
        if (!path.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return sign_futures(net::HttpMethod::Get, path.view(), {}, {}, now);
    }
    case MarketType::Spot: {
        if (symbol.empty()) {
            // /api/v3/openOrders has no account-wide form; answering with one
            // symbol's orders would read as "these are all of them".
            return std::unexpected{BuildError::NeedsSymbol};
        }
        Appender query{query_};
        query << "symbol=" << symbol;
        if (!query.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return sign_spot(net::HttpMethod::Get, ep::k_spot_open_orders, query.view(), now);
    }
    case MarketType::Future:
        return std::unexpected{BuildError::DatedFuturesUnsupported};
    case MarketType::Option:
        return std::unexpected{BuildError::OptionsUnsupported};
    }
    return std::unexpected{BuildError::UnknownInstrument};
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::query_order(
    const ExchangeClientOrderId& id, MarketType market, std::string_view symbol, UnixNanos now) {
    const auto coid = id.view();
    if (coid.empty()) {
        return std::unexpected{BuildError::NoOrderTarget};
    }
    switch (market) {
    case MarketType::Perp: {
        if (symbol.empty()) {
            return std::unexpected{BuildError::NeedsSymbol};
        }
        Appender path{path_};
        path << ep::k_futures_order_by_client_id << "/" << symbol << "/" << coid;
        if (!path.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return sign_futures(net::HttpMethod::Get, path.view(), {}, {}, now);
    }
    case MarketType::Spot: {
        // Unlike /api/v3/openOrders, the single-order read marks `symbol`
        // optional, so a client order id alone resolves an order the caller
        // has no pair for.
        Appender query{query_};
        query << "origClientOrderId=" << coid;
        if (!symbol.empty()) {
            query << "&symbol=" << symbol;
        }
        if (!query.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return sign_spot(net::HttpMethod::Get, ep::k_spot_order, query.view(), now);
    }
    case MarketType::Future:
        return std::unexpected{BuildError::DatedFuturesUnsupported};
    case MarketType::Option:
        return std::unexpected{BuildError::OptionsUnsupported};
    }
    return std::unexpected{BuildError::UnknownInstrument};
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::my_trades(MarketType market,
                                                                            std::string_view symbol,
                                                                            UnixNanos since,
                                                                            UnixNanos now) {
    const auto since_ms = static_cast<std::int64_t>(since.ns / 1'000'000ULL);
    switch (market) {
    case MarketType::Perp: {
        // Sorted by parameter name ascending: the futures digest covers the
        // query string in that order, and MEXC re-sorts before verifying.
        Appender query{query_};
        query << "page_num=1&page_size=" << k_page_size;
        if (!symbol.empty()) {
            query << "&symbol=" << symbol;
        }
        if (since_ms > 0) {
            query << "&start_time=" << since_ms;
        }
        if (!query.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return sign_futures(net::HttpMethod::Get, ep::k_futures_order_deals, query.view(), {}, now);
    }
    case MarketType::Spot: {
        if (symbol.empty()) {
            return std::unexpected{BuildError::NeedsSymbol};
        }
        Appender query{query_};
        query << "symbol=" << symbol << "&limit=" << k_page_size;
        if (since_ms > 0) {
            query << "&startTime=" << since_ms;
        }
        if (!query.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return sign_spot(net::HttpMethod::Get, ep::k_spot_my_trades, query.view(), now);
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

std::expected<net::RestRequest, BuildError>
SignedRequestBuilder::funding_records(MarketType market, UnixNanos since, UnixNanos now) {
    switch (market) {
    case MarketType::Perp: {
        // Sorted by parameter name ascending, like every other futures read:
        // the digest covers the query string in that order.
        Appender query{query_};
        query << "page_num=1&page_size=" << k_page_size;
        if (const auto since_ms = static_cast<std::int64_t>(since.ns / 1'000'000ULL);
            since_ms > 0) {
            query << "&start_time=" << since_ms;
        }
        if (!query.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return sign_futures(
            net::HttpMethod::Get, ep::k_futures_funding_records, query.view(), {}, now);
    }
    case MarketType::Spot:
        // A spot pair pays no funding, and this ledger is the FUTURES
        // account's. Answering with it would book a perpetual's charges
        // against a spot gateway.
        return std::unexpected{BuildError::SpotFundingUnsupported};
    case MarketType::Future:
        return std::unexpected{BuildError::DatedFuturesUnsupported};
    case MarketType::Option:
        return std::unexpected{BuildError::OptionsUnsupported};
    }
    return std::unexpected{BuildError::UnknownInstrument};
}

} // namespace raikiri::venues::mexc
