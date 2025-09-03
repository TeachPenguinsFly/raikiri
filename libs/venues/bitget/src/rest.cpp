#include <raikiri/net/json_writer.hpp>
#include <raikiri/venues/bitget/rest.hpp>

#include <charconv>
#include <cstring>

namespace raikiri::venues::bitget {

namespace {

constexpr std::string_view k_host = "api.bitget.com";
constexpr std::string_view k_product_live = "USDT-FUTURES";
constexpr std::string_view k_product_demo = "SUSDT-FUTURES";

// Header names are Bitget's, hyphens and all. The error strings say
// ACCESS_KEY with an underscore; the wire wants the hyphen.
constexpr std::string_view k_hdr_key = "ACCESS-KEY";
constexpr std::string_view k_hdr_sign = "ACCESS-SIGN";
constexpr std::string_view k_hdr_timestamp = "ACCESS-TIMESTAMP";
constexpr std::string_view k_hdr_passphrase = "ACCESS-PASSPHRASE";
constexpr std::string_view k_hdr_locale = "locale";

// Response-message language only, but it sits inside the documented "must
// contain" header block, so it is sent rather than argued with.
constexpr std::string_view k_locale = "en-US";

// One-way position mode, cross margin. tradeSide belongs to hedge mode and
// reduceOnly only works outside it, so an account in hedge mode will see
// every order refused with 45021 rather than opening the wrong leg.
constexpr std::string_view k_margin_mode = "crossed";

// Bitget caps a page at 100 on every list endpoint here.
constexpr std::string_view k_page_limit = "100";

// Fills the builder's target buffer. A part that would overflow is dropped
// rather than truncated mid-token, so an over-long symbol yields a request
// Bitget refuses with 40034 instead of one that names a different contract.
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
// truncated signed payload authenticates as something we did not mean to send.
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

    Appender& operator<<(std::uint64_t value) noexcept {
        std::array<char, 24> num{};
        const auto [stop, ec] = std::to_chars(num.data(), num.data() + num.size(), value);
        if (ec != std::errc{}) {
            overflow_ = true;
            return *this;
        }
        return *this << std::string_view{num.data(), static_cast<std::size_t>(stop - num.data())};
    }

    [[nodiscard]] bool ok() const noexcept { return !overflow_; }

    [[nodiscard]] std::string_view view() const noexcept { return {buf_.data(), len_}; }

private:
    std::span<char> buf_;
    std::size_t len_{0};
    bool overflow_{false};
};

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

// v2 calls time-in-force `force` on BOTH APIs and shares one value set;
// `timeInForceValue` was v1 and is gone. Post-only is a force value here, not
// a separate flag — sending it as a boolean is silently ignored and the order
// crosses.
[[nodiscard]] std::string_view force_text(TimeInForce tif, OrderFlags flags) noexcept {
    if (has_flag(flags, OrderFlags::PostOnly)) {
        return "post_only";
    }
    switch (tif) {
    case TimeInForce::Ioc:
        return "ioc";
    case TimeInForce::Fok:
        return "fok";
    case TimeInForce::Gtc:
        break;
    }
    return "gtc";
}

} // namespace

std::string_view to_string(BuildError err) noexcept {
    switch (err) {
    case BuildError::UnknownInstrument:
        return "unknown instrument";
    case BuildError::UnsupportedMarket:
        return "market not served by this adapter";
    case BuildError::UnsupportedOrder:
        return "order shape bitget cannot express";
    case BuildError::BadValue:
        return "bad price or quantity";
    case BuildError::BufferTooSmall:
        return "buffer too small";
    case BuildError::NotSigned:
        return "credentials not loaded";
    case BuildError::NothingToSend:
        return "empty batch";
    }
    return "unknown";
}

std::string_view rest_host() noexcept {
    return k_host;
}

std::string_view product_type(bool testnet) noexcept {
    return testnet ? k_product_demo : k_product_live;
}

std::string_view margin_coin(std::string_view product) noexcept {
    // "USDT-FUTURES" settles in USDT, "SUSDT-FUTURES" in the demo SUSDT. The
    // catalogue row carries no settle coin, so the product is the only source.
    const auto dash = product.find('-');
    return dash == std::string_view::npos ? product : product.substr(0, dash);
}

net::RestRequest RequestBuilder::server_time() noexcept {
    return {.method = net::HttpMethod::Get, .target = "/api/v2/public/time", .body = {}};
}

// Every public builder below writes into the same buffer, so a request must be
// sent before the next one is built. One catalogue load at a time is the only use.
net::RestRequest RequestBuilder::contracts(std::string_view product) noexcept {
    TargetWriter writer{target_};
    writer.put("/api/v2/mix/market/contracts?productType=");
    writer.put(product);
    return writer.get();
}

net::RestRequest RequestBuilder::spot_symbols() noexcept {
    return {.method = net::HttpMethod::Get, .target = "/api/v2/spot/public/symbols", .body = {}};
}

net::RestRequest RequestBuilder::ticker(std::string_view symbol,
                                        std::string_view product) noexcept {
    TargetWriter writer{target_};
    writer.put("/api/v2/mix/market/ticker?symbol=");
    writer.put(symbol);
    writer.put("&productType=");
    writer.put(product);
    return writer.get();
}

net::RestRequest RequestBuilder::current_fund_rate(std::string_view symbol,
                                                   std::string_view product) noexcept {
    TargetWriter writer{target_};
    writer.put("/api/v2/mix/market/current-fund-rate?symbol=");
    writer.put(symbol);
    writer.put("&productType=");
    writer.put(product);
    return writer.get();
}

net::RestRequest RequestBuilder::orderbook(std::string_view symbol,
                                           std::string_view product,
                                           std::uint32_t depth) noexcept {
    TargetWriter writer{target_};
    writer.put("/api/v2/mix/market/orderbook?symbol=");
    writer.put(symbol);
    writer.put("&productType=");
    writer.put(product);
    writer.put("&limit=");
    writer.put(depth);
    return writer.get();
}

void RequestBuilder::set_credentials(const net::Signer& signer,
                                     std::string_view passphrase) noexcept {
    signer_ = &signer;
    passphrase_ = passphrase;
}

std::string_view RequestBuilder::last_sign_message() const noexcept {
    return {sign_msg_.data(), sign_msg_len_};
}

std::expected<net::RestRequest, BuildError> RequestBuilder::sign(net::HttpMethod method,
                                                                 std::string_view path,
                                                                 std::string_view query,
                                                                 std::string_view body,
                                                                 UnixNanos now) {
    // The passphrase is as load-bearing as the secret: without it Bitget
    // answers 40011, which is not the code a missing key produces.
    if (signer_ == nullptr || !signer_->has_credentials() || passphrase_.empty()) {
        return std::unexpected{BuildError::NotSigned};
    }
    Appender ts{timestamp_};
    ts << now.ns / 1'000'000ULL;
    if (!ts.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }

    // The "?" belongs to the signer, not to the query: a query carrying its
    // own leading "?" doubles it here and answers 40009.
    Appender msg{sign_msg_};
    msg << ts.view() << method_text(method) << path;
    if (!query.empty()) {
        msg << "?" << query;
    }
    msg << body;
    if (!msg.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    sign_msg_len_ = msg.view().size();

    const auto signature = signer_->sign(msg.view(), net::Signer::Encoding::Base64, signature_);
    if (signature.empty()) {
        return std::unexpected{BuildError::NotSigned};
    }

    Appender target{signed_target_};
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
    request.headers = {{k_hdr_key, signer_->api_key()},
                       {k_hdr_sign, signature},
                       {k_hdr_timestamp, ts.view()},
                       {k_hdr_passphrase, passphrase_},
                       {k_hdr_locale, k_locale}};
    return request;
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::signed_post(std::string_view path, std::string_view body, UnixNanos now) {
    return sign(net::HttpMethod::Post, path, {}, body, now);
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::signed_get(std::string_view path, std::string_view query, UnixNanos now) {
    return sign(net::HttpMethod::Get, path, query, {}, now);
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::create_order(const OrderCmd& cmd,
                             const exec::InstrumentTable& table,
                             std::string_view product,
                             UnixNanos now) {
    const auto* instrument = table.find(cmd.instrument);
    if (instrument == nullptr) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    const auto& base = base_of(*instrument);
    if (cmd.qty.is_undef() || (cmd.order_type == OrderType::Limit && cmd.price.is_undef())) {
        return std::unexpected{BuildError::BadValue};
    }
    // A market order that must not cross is a contradiction, and `force`
    // carries both meanings here, so it cannot be expressed at all.
    if (cmd.order_type == OrderType::Market && has_flag(cmd.flags, OrderFlags::PostOnly)) {
        return std::unexpected{BuildError::UnsupportedOrder};
    }
    const auto symbol = base.exchange_symbol.view();
    const auto force = force_text(cmd.tif, cmd.flags);

    net::JsonWriter json{body_};
    switch (cmd.instrument.market()) {
    case MarketType::Spot: {
        // Spot `size` is base coin EXCEPT on a market buy, where it is the
        // quote coin. An OrderCmd carries lots of the base, so there is no
        // honest rendering of that one shape.
        if (cmd.order_type == OrderType::Market && cmd.side == Side::Buy) {
            return std::unexpected{BuildError::UnsupportedOrder};
        }
        json.obj_begin()
            .str("symbol", symbol)
            .str("side", side_text(cmd.side))
            .str("orderType", cmd.order_type == OrderType::Market ? "market" : "limit")
            // Documented required on spot whatever the order type, and ignored
            // for market orders; mix documents the opposite. Each follows its own.
            .str("force", force);
        if (cmd.order_type == OrderType::Limit) {
            json.decimal("price", base.ticks_to_price(cmd.price));
        }
        json.decimal("size", base.lots_to_qty(cmd.qty))
            .str("clientOid", cmd.exchange_client_order_id.view())
            .obj_end();
        break;
    }
    case MarketType::Perp: {
        json.obj_begin()
            .str("symbol", symbol)
            .str("productType", product)
            .str("marginMode", k_margin_mode)
            .str("marginCoin", margin_coin(product))
            .decimal("size", base.lots_to_qty(cmd.qty))
            .str("side", side_text(cmd.side))
            .str("orderType", cmd.order_type == OrderType::Market ? "market" : "limit");
        if (cmd.order_type == OrderType::Limit) {
            json.decimal("price", base.ticks_to_price(cmd.price)).str("force", force);
        }
        if (has_flag(cmd.flags, OrderFlags::ReduceOnly)) {
            // Only honoured in one-way mode, which is the mode this adapter
            // assumes by never sending tradeSide.
            json.str("reduceOnly", "YES");
        }
        json.str("clientOid", cmd.exchange_client_order_id.view()).obj_end();
        break;
    }
    case MarketType::Future:
        // Delivery contracts use these very endpoints, but map_instrument
        // reads symbolType "perpetual" only, so none can reach this call.
        return std::unexpected{BuildError::UnsupportedMarket};
    case MarketType::Option:
        // Bitget v2 has no crypto options at all; its only options product is
        // US equity options behind the separate v3 UTA API.
        return std::unexpected{BuildError::UnsupportedMarket};
    }

    if (!json.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return signed_post(cmd.instrument.market() == MarketType::Spot
                           ? "/api/v2/spot/trade/place-order"
                           : "/api/v2/mix/order/place-order",
                       json.view(),
                       now);
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::cancel_order(const OrderCmd& cmd,
                             const exec::InstrumentTable& table,
                             std::string_view product,
                             UnixNanos now) {
    const auto symbol = table.exchange_symbol(cmd.instrument);
    if (symbol.empty()) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    std::string_view path;
    net::JsonWriter json{body_};
    switch (cmd.instrument.market()) {
    case MarketType::Spot:
        path = "/api/v2/spot/trade/cancel-order";
        json.obj_begin().str("symbol", symbol);
        break;
    case MarketType::Perp:
        path = "/api/v2/mix/order/cancel-order";
        json.obj_begin().str("symbol", symbol).str("productType", product);
        break;
    case MarketType::Future:
        return std::unexpected{BuildError::UnsupportedMarket};
    case MarketType::Option:
        // No crypto options exist to cancel.
        return std::unexpected{BuildError::UnsupportedMarket};
    }
    // orderId wins when both are sent, so only one goes on the wire.
    if (!cmd.target_exchange_order_id.empty()) {
        json.str("orderId", cmd.target_exchange_order_id.view());
    } else {
        json.str("clientOid", cmd.exchange_client_order_id.view());
    }
    json.obj_end();

    if (!json.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return signed_post(path, json.view(), now);
}

std::expected<RequestBuilder::Batch, BuildError>
RequestBuilder::cancel_batch(std::span<const OrderCmd> cmds,
                             const exec::InstrumentTable& table,
                             std::string_view product,
                             UnixNanos now) {
    if (cmds.empty()) {
        return std::unexpected{BuildError::NothingToSend};
    }
    const auto instrument = cmds.front().instrument;
    const auto symbol = table.exchange_symbol(instrument);
    if (symbol.empty()) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    std::size_t batch = 0;
    while (batch < cmds.size() && batch < k_max_cancel_batch &&
           cmds[batch].instrument == instrument) {
        ++batch;
    }

    std::string_view path;
    std::string_view list_key;
    net::JsonWriter json{body_};
    switch (instrument.market()) {
    case MarketType::Spot:
        path = "/api/v2/spot/trade/batch-cancel-order";
        list_key = "orderList";
        json.obj_begin().str("symbol", symbol);
        break;
    case MarketType::Perp:
        path = "/api/v2/mix/order/batch-cancel-orders";
        list_key = "orderIdList";
        json.obj_begin().str("symbol", symbol).str("productType", product);
        break;
    case MarketType::Future:
        return std::unexpected{BuildError::UnsupportedMarket};
    case MarketType::Option:
        return std::unexpected{BuildError::UnsupportedMarket};
    }

    // Bitget refuses a batch that mixes orderId with clientOid — the entries
    // carrying only a clientOid are silently left open — so every entry here
    // names the client id and nothing else.
    net::JsonWriter list{list_};
    list.arr_begin();
    for (std::size_t i = 0; i < batch; ++i) {
        list.obj_begin().str("clientOid", cmds[i].exchange_client_order_id.view()).obj_end();
    }
    list.arr_end();
    if (!list.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    json.raw(list_key, list.view()).obj_end();
    if (!json.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    const auto request = signed_post(path, json.view(), now);
    if (!request.has_value()) {
        return std::unexpected{request.error()};
    }
    return Batch{.request = *request, .consumed = batch};
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::open_orders(MarketType market, std::string_view product, UnixNanos now) {
    Appender query{body_}; // a GET sends no body; the buffer is free
    switch (market) {
    case MarketType::Spot:
        query << "limit=" << k_page_limit;
        if (!query.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return signed_get("/api/v2/spot/trade/unfilled-orders", query.view(), now);
    case MarketType::Perp:
        query << "limit=" << k_page_limit << "&productType=" << product;
        if (!query.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return signed_get("/api/v2/mix/order/orders-pending", query.view(), now);
    case MarketType::Future:
        return std::unexpected{BuildError::UnsupportedMarket};
    case MarketType::Option:
        return std::unexpected{BuildError::UnsupportedMarket};
    }
    return std::unexpected{BuildError::UnsupportedMarket};
}

std::expected<net::RestRequest, BuildError> RequestBuilder::order_by_client_id(
    const ExchangeClientOrderId& id, MarketType market, std::string_view product, UnixNanos now) {
    if (id.empty()) {
        return std::unexpected{BuildError::BadValue};
    }
    Appender query{body_};
    switch (market) {
    case MarketType::Spot:
        query << "clientOid=" << id.view();
        if (!query.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return signed_get("/api/v2/spot/trade/orderInfo", query.view(), now);
    case MarketType::Perp:
        query << "clientOid=" << id.view() << "&productType=" << product;
        if (!query.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return signed_get("/api/v2/mix/order/orders-pending", query.view(), now);
    case MarketType::Future:
        return std::unexpected{BuildError::UnsupportedMarket};
    case MarketType::Option:
        return std::unexpected{BuildError::UnsupportedMarket};
    }
    return std::unexpected{BuildError::UnsupportedMarket};
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::fills(MarketType market, std::string_view product, UnixNanos since, UnixNanos now) {
    // Keys ascend: Bitget's own signing sample sorts the query, and the only
    // way to satisfy both that and "sign what you send" is to send it sorted.
    Appender query{body_};
    query << "limit=" << k_page_limit;
    switch (market) {
    case MarketType::Spot:
        if (since.ns != 0) {
            query << "&startTime=" << since.ns / 1'000'000ULL;
        }
        if (!query.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return signed_get("/api/v2/spot/trade/fills", query.view(), now);
    case MarketType::Perp:
        query << "&productType=" << product;
        if (since.ns != 0) {
            query << "&startTime=" << since.ns / 1'000'000ULL;
        }
        if (!query.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return signed_get("/api/v2/mix/order/fills", query.view(), now);
    case MarketType::Future:
        return std::unexpected{BuildError::UnsupportedMarket};
    case MarketType::Option:
        return std::unexpected{BuildError::UnsupportedMarket};
    }
    return std::unexpected{BuildError::UnsupportedMarket};
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::positions(MarketType market, std::string_view product, UnixNanos now) {
    switch (market) {
    case MarketType::Spot:
        // Spot holds coin balances, not per-instrument positions: there is
        // nothing here to report against an InstrumentId.
        return std::unexpected{BuildError::UnsupportedMarket};
    case MarketType::Perp: {
        Appender query{body_};
        query << "productType=" << product;
        if (!query.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return signed_get("/api/v2/mix/position/all-position", query.view(), now);
    }
    case MarketType::Future:
        return std::unexpected{BuildError::UnsupportedMarket};
    case MarketType::Option:
        return std::unexpected{BuildError::UnsupportedMarket};
    }
    return std::unexpected{BuildError::UnsupportedMarket};
}

} // namespace raikiri::venues::bitget
