#include <raikiri/net/json_writer.hpp>
#include <raikiri/venues/bitget/endpoints.hpp>
#include <raikiri/venues/bitget/rest.hpp>

#include <charconv>
#include <cstring>

namespace raikiri::venues::bitget {

namespace {

constexpr std::string_view k_product_live = "USDT-FUTURES";
constexpr std::string_view k_product_demo = "SUSDT-FUTURES";

// header names use hyphens; the error strings spell them with underscores
constexpr std::string_view k_hdr_key = "ACCESS-KEY";
constexpr std::string_view k_hdr_sign = "ACCESS-SIGN";
constexpr std::string_view k_hdr_timestamp = "ACCESS-TIMESTAMP";
constexpr std::string_view k_hdr_passphrase = "ACCESS-PASSPHRASE";
constexpr std::string_view k_hdr_locale = "locale";

constexpr std::string_view k_locale = "en-US";

// one-way mode, cross margin; a hedge-mode account gets every order refused with 45021
constexpr std::string_view k_margin_mode = "crossed";

// bitget caps a page at 100 on these list endpoints
constexpr std::string_view k_page_limit = "100";

constexpr std::string_view k_settle_fee_type = "contract_settle_fee";

// an overflowing part is dropped whole, so the venue refuses the request instead of reading another contract
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

// reports overflow rather than truncating a signed payload
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

// post-only is a force value; a boolean flag is ignored and the order crosses
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
    return ep::rest_host();
}

std::string_view product_type(bool testnet) noexcept {
    return testnet ? k_product_demo : k_product_live;
}

std::string_view margin_coin(std::string_view product) noexcept {
    // the settle coin is the product name's prefix before the dash
    const auto dash = product.find('-');
    return dash == std::string_view::npos ? product : product.substr(0, dash);
}

net::RestRequest RequestBuilder::server_time() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_server_time, .body = {}};
}

// all public builders share one buffer; send each request before building the next
net::RestRequest RequestBuilder::contracts(std::string_view product) noexcept {
    TargetWriter writer{target_};
    writer.put(ep::k_mix_contracts);
    writer.put("?productType=");
    writer.put(product);
    return writer.get();
}

net::RestRequest RequestBuilder::spot_symbols() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_spot_symbols, .body = {}};
}

net::RestRequest RequestBuilder::ticker(std::string_view symbol, std::string_view product) noexcept {
    TargetWriter writer{target_};
    writer.put(ep::k_mix_ticker);
    writer.put("?symbol=");
    writer.put(symbol);
    writer.put("&productType=");
    writer.put(product);
    return writer.get();
}

net::RestRequest RequestBuilder::current_fund_rate(std::string_view symbol, std::string_view product) noexcept {
    TargetWriter writer{target_};
    writer.put(ep::k_mix_fund_rate);
    writer.put("?symbol=");
    writer.put(symbol);
    writer.put("&productType=");
    writer.put(product);
    return writer.get();
}

net::RestRequest
RequestBuilder::orderbook(std::string_view symbol, std::string_view product, std::uint32_t depth) noexcept {
    TargetWriter writer{target_};
    writer.put(ep::k_mix_orderbook);
    writer.put("?symbol=");
    writer.put(symbol);
    writer.put("&productType=");
    writer.put(product);
    writer.put("&limit=");
    writer.put(depth);
    return writer.get();
}

void RequestBuilder::set_credentials(const net::Signer& signer, std::string_view passphrase) noexcept {
    signer_ = &signer;
    passphrase_ = passphrase;
}

std::string_view RequestBuilder::last_sign_message() const noexcept {
    return {sign_msg_.data(), sign_msg_len_};
}

std::expected<net::RestRequest, BuildError> RequestBuilder::sign(
    net::HttpMethod method, std::string_view path, std::string_view query, std::string_view body, UnixNanos now) {
    // without the passphrase bitget answers 40011, not the missing-key code
    if (signer_ == nullptr || !signer_->has_credentials() || passphrase_.empty()) {
        return std::unexpected{BuildError::NotSigned};
    }
    Appender ts{timestamp_};
    ts << now.ns / 1'000'000ULL;
    if (!ts.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }

    // the signer adds the "?"; a query with its own doubles it and gets 40009
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

std::expected<net::RestRequest, BuildError> RequestBuilder::create_order(const OrderCmd& cmd,
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
    // force carries both post-only and tif, so a post-only market order cannot be expressed
    if (cmd.order_type == OrderType::Market && has_flag(cmd.flags, OrderFlags::PostOnly)) {
        return std::unexpected{BuildError::UnsupportedOrder};
    }
    const auto symbol = base.exchange_symbol.view();
    const auto force = force_text(cmd.tif, cmd.flags);

    net::JsonWriter json{body_};
    switch (cmd.instrument.market()) {
    case MarketType::Spot: {
        // spot market buys size in the quote coin, which an OrderCmd cannot express
        if (cmd.order_type == OrderType::Market && cmd.side == Side::Buy) {
            return std::unexpected{BuildError::UnsupportedOrder};
        }
        json.obj_begin()
            .str("symbol", symbol)
            .str("side", side_text(cmd.side))
            .str("orderType", cmd.order_type == OrderType::Market ? "market" : "limit")
            // required on spot for every order type; mix documents the opposite
            .str("force", force);
        if (cmd.order_type == OrderType::Limit) {
            json.decimal("price", base.ticks_to_price(cmd.price));
        }
        json.decimal("size", base.lots_to_qty(cmd.qty)).str("clientOid", cmd.exchange_client_order_id.view()).obj_end();
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
            // honoured only in one-way mode, which this adapter assumes
            json.str("reduceOnly", "YES");
        }
        json.str("clientOid", cmd.exchange_client_order_id.view()).obj_end();
        break;
    }
    case MarketType::Future:
        // map_instrument reads perpetual rows only, so delivery never reaches here
        return std::unexpected{BuildError::UnsupportedMarket};
    case MarketType::Option:
        // bitget v2 has no crypto options
        return std::unexpected{BuildError::UnsupportedMarket};
    }

    if (!json.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return signed_post(
        cmd.instrument.market() == MarketType::Spot ? ep::k_spot_place_order : ep::k_mix_place_order, json.view(), now);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::cancel_order(const OrderCmd& cmd,
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
        path = ep::k_spot_cancel_order;
        json.obj_begin().str("symbol", symbol);
        break;
    case MarketType::Perp:
        path = ep::k_mix_cancel_order;
        json.obj_begin().str("symbol", symbol).str("productType", product);
        break;
    case MarketType::Future:
        return std::unexpected{BuildError::UnsupportedMarket};
    case MarketType::Option:
        return std::unexpected{BuildError::UnsupportedMarket};
    }
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
    return signed_post(path, json.view(), now);
}

std::expected<RequestBuilder::Batch, BuildError> RequestBuilder::cancel_batch(std::span<const OrderCmd> cmds,
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
    while (batch < cmds.size() && batch < k_max_cancel_batch && cmds[batch].instrument == instrument) {
        ++batch;
    }

    std::string_view path;
    std::string_view list_key;
    net::JsonWriter json{body_};
    switch (instrument.market()) {
    case MarketType::Spot:
        path = ep::k_spot_batch_cancel;
        list_key = "orderList";
        json.obj_begin().str("symbol", symbol);
        break;
    case MarketType::Perp:
        path = ep::k_mix_batch_cancel;
        list_key = "orderIdList";
        json.obj_begin().str("symbol", symbol).str("productType", product);
        break;
    case MarketType::Future:
        return std::unexpected{BuildError::UnsupportedMarket};
    case MarketType::Option:
        return std::unexpected{BuildError::UnsupportedMarket};
    }

    // a batch mixing orderId and clientOid leaves the clientOid entries open, so only clientOid is sent
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
        return signed_get(ep::k_spot_unfilled_orders, query.view(), now);
    case MarketType::Perp:
        query << "limit=" << k_page_limit << "&productType=" << product;
        if (!query.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return signed_get(ep::k_mix_orders_pending, query.view(), now);
    case MarketType::Future:
        return std::unexpected{BuildError::UnsupportedMarket};
    case MarketType::Option:
        return std::unexpected{BuildError::UnsupportedMarket};
    }
    return std::unexpected{BuildError::UnsupportedMarket};
}

std::expected<net::RestRequest, BuildError> RequestBuilder::order_by_client_id(const ExchangeClientOrderId& id,
                                                                               MarketType market,
                                                                               std::string_view product,
                                                                               UnixNanos now) {
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
        return signed_get(ep::k_spot_order_info, query.view(), now);
    case MarketType::Perp:
        query << "clientOid=" << id.view() << "&productType=" << product;
        if (!query.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return signed_get(ep::k_mix_orders_pending, query.view(), now);
    case MarketType::Future:
        return std::unexpected{BuildError::UnsupportedMarket};
    case MarketType::Option:
        return std::unexpected{BuildError::UnsupportedMarket};
    }
    return std::unexpected{BuildError::UnsupportedMarket};
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::fills(MarketType market, std::string_view product, UnixNanos since, UnixNanos now) {
    // keys sorted ascending, as bitget's signing sample expects
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
        return signed_get(ep::k_spot_fills, query.view(), now);
    case MarketType::Perp:
        query << "&productType=" << product;
        if (since.ns != 0) {
            query << "&startTime=" << since.ns / 1'000'000ULL;
        }
        if (!query.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return signed_get(ep::k_mix_fills, query.view(), now);
    case MarketType::Future:
        return std::unexpected{BuildError::UnsupportedMarket};
    case MarketType::Option:
        return std::unexpected{BuildError::UnsupportedMarket};
    }
    return std::unexpected{BuildError::UnsupportedMarket};
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::settle_bills(MarketType market, std::string_view product, UnixNanos since, UnixNanos now) {
    switch (market) {
    case MarketType::Perp: {
        // keys ascend; businessType filters to contract_settle_fee at the venue
        Appender query{body_};
        query << "businessType=" << k_settle_fee_type << "&limit=" << k_page_limit << "&productType=" << product;
        // bitget refuses windows over 30 days; no startTime asks for the newest page
        if (since.ns != 0) {
            query << "&startTime=" << since.ns / 1'000'000ULL;
        }
        if (!query.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return signed_get(ep::k_mix_account_bill, query.view(), now);
    }
    case MarketType::Spot:
    case MarketType::Future:
    case MarketType::Option:
        return std::unexpected{BuildError::UnsupportedMarket};
    }
    return std::unexpected{BuildError::UnsupportedMarket};
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::positions(MarketType market, std::string_view product, UnixNanos now) {
    switch (market) {
    case MarketType::Spot:
        // spot holds coin balances, not per-instrument positions
        return std::unexpected{BuildError::UnsupportedMarket};
    case MarketType::Perp: {
        Appender query{body_};
        query << "productType=" << product;
        if (!query.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        return signed_get(ep::k_mix_positions, query.view(), now);
    }
    case MarketType::Future:
        return std::unexpected{BuildError::UnsupportedMarket};
    case MarketType::Option:
        return std::unexpected{BuildError::UnsupportedMarket};
    }
    return std::unexpected{BuildError::UnsupportedMarket};
}

} // namespace raikiri::venues::bitget
