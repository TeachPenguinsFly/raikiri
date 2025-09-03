#include <raikiri/net/json_writer.hpp>
#include <raikiri/venues/kucoin/rest.hpp>

#include <charconv>
#include <cstring>
#include <span>

namespace raikiri::venues::kucoin {

namespace {

constexpr std::string_view k_host = "api-futures.kucoin.com";
constexpr std::string_view k_host_spot = "api.kucoin.com";

// Header names are KuCoin's, capitalisation and all.
constexpr std::string_view k_hdr_key = "KC-API-KEY";
constexpr std::string_view k_hdr_sign = "KC-API-SIGN";
constexpr std::string_view k_hdr_ts = "KC-API-TIMESTAMP";
constexpr std::string_view k_hdr_passphrase = "KC-API-PASSPHRASE";
constexpr std::string_view k_hdr_key_version = "KC-API-KEY-VERSION";
constexpr std::string_view k_key_version = "2";

// Fills the builder's target buffer. A part that would overflow is dropped
// rather than truncated mid-token, so an over-long symbol yields a request
// KuCoin refuses with 404000 instead of one that names a different contract.
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

    [[nodiscard]] net::RestRequest get() const noexcept {
        return {.method = net::HttpMethod::Get,
                .target = std::string_view{into_.data(), len_},
                .body = {}};
    }

private:
    std::span<char> into_;
    std::size_t len_{0};
};

// The signed path cannot use TargetWriter: a truncated path is signed as, and
// sent as, a request we did not mean to make. Overflow is sticky and reported.
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

std::string_view render_u64(std::uint64_t value, std::span<char> out) noexcept {
    const auto [end, ec] = std::to_chars(out.data(), out.data() + out.size(), value);
    return ec == std::errc{}
               ? std::string_view{out.data(), static_cast<std::size_t>(end - out.data())}
               : std::string_view{};
}

[[nodiscard]] std::string_view method_text(net::HttpMethod method) noexcept {
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

[[nodiscard]] std::string_view side_text(Side side) noexcept {
    return side == Side::Buy ? "buy" : "sell";
}

[[nodiscard]] std::string_view type_text(OrderType type) noexcept {
    return type == OrderType::Market ? "market" : "limit";
}

[[nodiscard]] std::string_view tif_text(TimeInForce tif) noexcept {
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

// KuCoin's clientOid alphabet is letters, digits, '_' and '-'. A value outside
// it is accepted by the venue on some paths and silently mangled on others, so
// it is refused here rather than sent.
[[nodiscard]] bool client_oid_usable(std::string_view id) noexcept {
    if (id.empty() || id.size() > k_max_client_oid) {
        return false;
    }
    for (const char c : id) {
        const bool alnum =
            (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        if (!alnum && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

} // namespace

std::string_view to_string(BuildError err) noexcept {
    switch (err) {
    case BuildError::UnknownInstrument:
        return "unknown instrument";
    case BuildError::BadQuantity:
        return "bad quantity";
    case BuildError::BadPrice:
        return "bad price";
    case BuildError::BadClientOrderId:
        return "bad client order id";
    case BuildError::UnsupportedMarket:
        return "unsupported market";
    case BuildError::UnsupportedRequest:
        return "unsupported request";
    case BuildError::BufferTooSmall:
        return "buffer too small";
    case BuildError::NotSigned:
        return "credentials not loaded";
    }
    return "unknown";
}

std::string_view rest_host() noexcept {
    return k_host;
}

std::string_view rest_host(MarketType market) noexcept {
    switch (market) {
    case MarketType::Spot:
        return k_host_spot;
    case MarketType::Perp:
    case MarketType::Future:
        // One host serves both: the perpetuals and the dated contracts share
        // /api/v1/contracts/active, and only `type` tells them apart.
        return k_host;
    case MarketType::Option:
        return {}; // KuCoin lists no options on the APIs in scope
    }
    return {};
}

net::RestRequest RequestBuilder::bullet_public() noexcept {
    return {.method = net::HttpMethod::Post, .target = "/api/v1/bullet-public", .body = {}};
}

net::RestRequest RequestBuilder::contracts_active() noexcept {
    return {.method = net::HttpMethod::Get, .target = "/api/v1/contracts/active", .body = {}};
}

net::RestRequest RequestBuilder::server_time() noexcept {
    return {.method = net::HttpMethod::Get, .target = "/api/v1/timestamp", .body = {}};
}

net::RestRequest RequestBuilder::spot_symbols() noexcept {
    return {.method = net::HttpMethod::Get, .target = "/api/v2/symbols", .body = {}};
}

// Every builder below writes into the same buffer, so a request must be sent
// before the next one is built. One cold read at a time is the only use.
net::RestRequest RequestBuilder::funding_rate_current(std::string_view symbol) noexcept {
    TargetWriter writer{target_};
    writer.put("/api/v1/funding-rate/");
    writer.put(symbol);
    writer.put("/current");
    return writer.get();
}

net::RestRequest RequestBuilder::level2_snapshot(std::string_view symbol) noexcept {
    TargetWriter writer{target_};
    writer.put("/api/v1/level2/snapshot?symbol=");
    writer.put(symbol);
    return writer.get();
}

bool Credentials::seal_passphrase(std::string_view raw) noexcept {
    passphrase_len_ = 0;
    if (raw.empty()) {
        return false;
    }
    const auto sealed = signer_.sign(raw, net::Signer::Encoding::Base64, passphrase_);
    passphrase_len_ = sealed.size();
    return passphrase_len_ != 0;
}

std::string_view OrderRequestBuilder::last_sign_message() const noexcept {
    return {sign_msg_.data(), sign_msg_len_};
}

std::expected<net::RestRequest, BuildError> OrderRequestBuilder::sign(net::HttpMethod method,
                                                                      std::string_view path,
                                                                      std::string_view body,
                                                                      UnixNanos now) {
    if (!credentials_->complete()) {
        return std::unexpected{BuildError::NotSigned};
    }
    const auto ts = render_u64(now.ns / 1'000'000ULL, timestamp_);

    // The signed string is the request line as it will be SENT: the method in
    // upper case, the path with its query string attached, and the body byte
    // for byte. Signing a sorted or re-encoded query authenticates a request
    // the server never sees, and answers 400005.
    Appender msg{sign_msg_};
    msg << ts << method_text(method) << path << body;
    if (!msg.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    sign_msg_len_ = msg.view().size();

    const auto signature =
        credentials_->signer().sign(msg.view(), net::Signer::Encoding::Base64, signature_);
    if (signature.empty()) {
        return std::unexpected{BuildError::NotSigned};
    }

    net::RestRequest request{};
    request.method = method;
    request.target = path;
    request.body = body;
    request.headers = {{k_hdr_key, credentials_->signer().api_key()},
                       {k_hdr_sign, signature},
                       {k_hdr_ts, ts},
                       {k_hdr_passphrase, credentials_->sealed_passphrase()},
                       {k_hdr_key_version, k_key_version}};
    return request;
}

std::expected<net::RestRequest, BuildError> OrderRequestBuilder::create_order(
    const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
    const auto market = cmd.instrument.market();
    // Named before the table is read, as amend_order does. InstrumentId
    // resolves by symbol index alone, so an option id whose index happens to
    // be occupied would otherwise be refused for the wrong reason.
    if (market == MarketType::Option) {
        return std::unexpected{BuildError::UnsupportedMarket};
    }
    const auto* instrument = table.find(cmd.instrument);
    if (instrument == nullptr) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    const auto& base = base_of(*instrument);
    const auto symbol = table.exchange_symbol(cmd.instrument);
    const auto client_oid = cmd.exchange_client_order_id.view();
    if (!client_oid_usable(client_oid)) {
        return std::unexpected{BuildError::BadClientOrderId};
    }
    if (cmd.qty.is_undef() || cmd.qty.raw <= 0) {
        return std::unexpected{BuildError::BadQuantity};
    }
    if (cmd.order_type == OrderType::Limit && cmd.price.is_undef()) {
        return std::unexpected{BuildError::BadPrice};
    }
    // Post-only is a separate boolean on KuCoin and is legal only alongside a
    // resting time in force. Sending it with IOC produces an order that is
    // neither, so the combination is refused rather than silently reshaped.
    const bool post_only = has_flag(cmd.flags, OrderFlags::PostOnly);
    if (post_only && cmd.tif != TimeInForce::Gtc) {
        return std::unexpected{BuildError::UnsupportedRequest};
    }

    net::JsonWriter json{body_};
    std::string_view path;
    switch (market) {
    case MarketType::Spot: {
        if (has_flag(cmd.flags, OrderFlags::ReduceOnly)) {
            // Spot holds no position to reduce; dropping the flag would send
            // an opening order where a closing one was asked for.
            return std::unexpected{BuildError::UnsupportedRequest};
        }
        path = "/api/v1/hf/orders";
        json.obj_begin()
            .str("clientOid", client_oid)
            .str("symbol", symbol)
            .str("side", side_text(cmd.side))
            .str("type", type_text(cmd.order_type))
            .decimal("size", base.lots_to_qty(cmd.qty));
        if (cmd.order_type == OrderType::Limit) {
            json.decimal("price", base.ticks_to_price(cmd.price))
                .str("timeInForce", tif_text(cmd.tif));
        }
        if (post_only) {
            json.boolean("postOnly", true);
        }
        json.obj_end();
        break;
    }
    case MarketType::Perp:
    case MarketType::Future: {
        // Futures trade whole CONTRACTS. A quantity that is not a whole
        // number of them is a schema disagreement, and rounding it is how an
        // order goes to the venue at a size nobody chose.
        const auto contracts = base.lots_to_qty(cmd.qty);
        if (contracts.raw <= 0 || contracts.raw % k_e9 != 0) {
            return std::unexpected{BuildError::BadQuantity};
        }
        path = "/api/v1/orders";
        json.obj_begin()
            .str("clientOid", client_oid)
            .str("symbol", symbol)
            .str("side", side_text(cmd.side))
            .str("type", type_text(cmd.order_type))
            .num("size", contracts.raw / k_e9);
        if (cmd.order_type == OrderType::Limit) {
            json.decimal("price", base.ticks_to_price(cmd.price))
                .str("timeInForce", tif_text(cmd.tif));
        }
        if (post_only) {
            json.boolean("postOnly", true);
        }
        if (has_flag(cmd.flags, OrderFlags::ReduceOnly)) {
            json.boolean("reduceOnly", true);
        }
        // `leverage` is deliberately absent. It is optional on this endpoint
        // and it SETS the position's margin under isolated mode, so inventing
        // a value here would silently choose risk the caller never asked for.
        json.obj_end();
        break;
    }
    case MarketType::Option:
        return std::unexpected{BuildError::UnsupportedMarket}; // KuCoin lists none
    }

    if (!json.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Post, path, json.view(), now);
}

std::expected<net::RestRequest, BuildError> OrderRequestBuilder::cancel_order(
    const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
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

    Appender path{path_};
    switch (market) {
    case MarketType::Spot:
        // Both spot cancel paths demand the symbol as a query parameter, and
        // it is part of the signed string.
        path << "/api/v1/hf/orders/";
        if (by_order_id) {
            path << cmd.target_exchange_order_id.view();
        } else {
            path << "client-order/" << client_oid;
        }
        path << "?symbol=" << symbol;
        break;
    case MarketType::Perp:
    case MarketType::Future:
        path << "/api/v1/orders/";
        if (by_order_id) {
            path << cmd.target_exchange_order_id.view();
        } else {
            path << "client-order/" << client_oid << "?symbol=" << symbol;
        }
        break;
    case MarketType::Option:
        return std::unexpected{BuildError::UnsupportedMarket};
    }
    if (!path.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Delete, path.view(), {}, now);
}

std::expected<net::RestRequest, BuildError> OrderRequestBuilder::amend_order(
    const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
    const auto market = cmd.instrument.market();
    switch (market) {
    case MarketType::Spot:
        break;
    case MarketType::Perp:
    case MarketType::Future:
        // The futures API has no modify at all; cancel-and-replace is a
        // decision for the OMS, not something an adapter should fake.
        return std::unexpected{BuildError::UnsupportedRequest};
    case MarketType::Option:
        return std::unexpected{BuildError::UnsupportedMarket};
    }

    const auto* instrument = table.find(cmd.instrument);
    if (instrument == nullptr) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    const auto& base = base_of(*instrument);
    const auto symbol = table.exchange_symbol(cmd.instrument);
    const auto client_oid = cmd.exchange_client_order_id.view();
    const auto by_order_id = !cmd.target_exchange_order_id.empty();
    if (!by_order_id && !client_oid_usable(client_oid)) {
        return std::unexpected{BuildError::BadClientOrderId};
    }
    // KuCoin's modify REPLACES both fields, so an amend naming only one cannot
    // be expressed. Filling the other from remembered state would resubmit a
    // price the venue may already have moved past.
    if (cmd.price.is_undef()) {
        return std::unexpected{BuildError::BadPrice};
    }
    if (cmd.qty.is_undef() || cmd.qty.raw <= 0) {
        return std::unexpected{BuildError::BadQuantity};
    }

    net::JsonWriter json{body_};
    json.obj_begin().str("symbol", symbol);
    if (by_order_id) {
        json.str("orderId", cmd.target_exchange_order_id.view());
    } else {
        json.str("clientOid", client_oid);
    }
    json.decimal("newPrice", base.ticks_to_price(cmd.price))
        .decimal("newSize", base.lots_to_qty(cmd.qty))
        .obj_end();
    if (!json.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Post, "/api/v1/hf/orders/alter", json.view(), now);
}

std::expected<net::RestRequest, BuildError>
OrderRequestBuilder::open_orders(MarketType market, std::string_view symbol, UnixNanos now) {
    Appender path{path_};
    switch (market) {
    case MarketType::Spot:
        if (symbol.empty()) {
            // KuCoin answers spot actives one symbol at a time; an empty
            // symbol would fetch nothing and read as "no open orders".
            return std::unexpected{BuildError::UnsupportedRequest};
        }
        path << "/api/v1/hf/orders/active?symbol=" << symbol;
        break;
    case MarketType::Perp:
    case MarketType::Future:
        // Without status=active KuCoin returns the DONE orders instead, which
        // a reconciler would read as a book full of orders that are gone.
        path << "/api/v1/orders?status=active";
        if (!symbol.empty()) {
            path << "&symbol=" << symbol;
        }
        break;
    case MarketType::Option:
        return std::unexpected{BuildError::UnsupportedMarket};
    }
    if (!path.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Get, path.view(), {}, now);
}

std::expected<net::RestRequest, BuildError> OrderRequestBuilder::active_symbols(MarketType market,
                                                                                UnixNanos now) {
    switch (market) {
    case MarketType::Spot:
        return sign(net::HttpMethod::Get, "/api/v1/hf/orders/active/symbols", {}, now);
    case MarketType::Perp:
    case MarketType::Future:
        // The futures list is account-wide already; asking per symbol would
        // turn one sweep into hundreds.
        return std::unexpected{BuildError::UnsupportedRequest};
    case MarketType::Option:
        return std::unexpected{BuildError::UnsupportedMarket};
    }
    return std::unexpected{BuildError::UnsupportedMarket};
}

std::expected<net::RestRequest, BuildError>
OrderRequestBuilder::fills(MarketType market, UnixNanos since, UnixNanos now) {
    std::array<char, 24> since_text{};
    const auto since_ms = render_u64(since.ns / 1'000'000ULL, since_text);

    Appender path{path_};
    switch (market) {
    case MarketType::Spot:
        path << "/api/v1/hf/fills";
        break;
    case MarketType::Perp:
    case MarketType::Future:
        path << "/api/v1/fills";
        break;
    case MarketType::Option:
        return std::unexpected{BuildError::UnsupportedMarket};
    }
    if (since.ns != 0) {
        path << "?startAt=" << since_ms;
    }
    if (!path.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Get, path.view(), {}, now);
}

std::expected<net::RestRequest, BuildError> OrderRequestBuilder::positions(MarketType market,
                                                                           UnixNanos now) {
    switch (market) {
    case MarketType::Spot:
        // Spot has balances, not positions. An empty list here would be read
        // as "flat", which is a claim about a concept the market lacks.
        return std::unexpected{BuildError::UnsupportedRequest};
    case MarketType::Perp:
    case MarketType::Future:
        return sign(net::HttpMethod::Get, "/api/v1/positions", {}, now);
    case MarketType::Option:
        return std::unexpected{BuildError::UnsupportedMarket};
    }
    return std::unexpected{BuildError::UnsupportedMarket};
}

std::expected<net::RestRequest, BuildError> OrderRequestBuilder::account_overview(MarketType market,
                                                                                  UnixNanos now) {
    switch (market) {
    case MarketType::Spot:
        // The spot wallet is a per-currency list on the other host. Refused,
        // because the futures overview describes a different account.
        return std::unexpected{BuildError::UnsupportedRequest};
    case MarketType::Perp:
    case MarketType::Future: {
        Appender path{path_};
        path << "/api/v1/account-overview?currency=" << k_settle_currency;
        if (!path.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return sign(net::HttpMethod::Get, path.view(), {}, now);
    }
    case MarketType::Option:
        return std::unexpected{BuildError::UnsupportedMarket};
    }
    return std::unexpected{BuildError::UnsupportedMarket};
}

std::expected<net::RestRequest, BuildError> OrderRequestBuilder::bullet_private(MarketType market,
                                                                                UnixNanos now) {
    switch (market) {
    case MarketType::Spot:
    case MarketType::Perp:
    case MarketType::Future:
        // Same path on both hosts; which host it reaches decides which
        // account's stream the token opens.
        return sign(net::HttpMethod::Post, "/api/v1/bullet-private", {}, now);
    case MarketType::Option:
        return std::unexpected{BuildError::UnsupportedMarket};
    }
    return std::unexpected{BuildError::UnsupportedMarket};
}

} // namespace raikiri::venues::kucoin
