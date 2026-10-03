#include <raikiri/net/json_writer.hpp>
#include <raikiri/venues/htx/endpoints.hpp>
#include <raikiri/venues/htx/rest.hpp>

#include <charconv>
#include <cstring>
#include <limits>

namespace raikiri::venues::htx {

namespace {

// SignatureVersion is "2" everywhere except the spot websocket handshake ("2.1")
constexpr std::string_view k_param_key = "AccessKeyId";
constexpr std::string_view k_param_method = "SignatureMethod";
constexpr std::string_view k_param_version = "SignatureVersion";
constexpr std::string_view k_param_timestamp = "Timestamp";
constexpr std::string_view k_signature_method = "HmacSHA256";
constexpr std::string_view k_signature_version = "2";

// both spot list endpoints cap at 500
constexpr std::string_view k_spot_page_limit = "500";

// htx refuses a start further back than 48 hours
constexpr std::uint64_t k_financial_record_window_ms = 48ULL * 60 * 60 * 1000;

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

[[nodiscard]] std::string_view render_i64(std::int64_t value, std::span<char> out) noexcept {
    const auto [stop, ec] = std::to_chars(out.data(), out.data() + out.size(), value);
    return ec == std::errc{} ? std::string_view{out.data(), static_cast<std::size_t>(stop - out.data())}
                             : std::string_view{};
}

// only limit, post_only, ioc and fok may carry a price
[[nodiscard]] std::string_view swap_order_price_type(const OrderCmd& cmd) noexcept {
    if (cmd.order_type == OrderType::Market) {
        switch (cmd.tif) {
        case TimeInForce::Fok:
            // fok needs a price, so the bbo-priced variant expresses a market fok
            return "opponent_fok";
        case TimeInForce::Ioc:
        case TimeInForce::Gtc:
            break;
        }
        // a market order never rests, so gtc and ioc are the same
        return "market";
    }
    if (has_flag(cmd.flags, OrderFlags::PostOnly)) {
        // the swap cancels a crossing post_only order (status 7 push); spot rejects it at submit
        return "post_only";
    }
    switch (cmd.tif) {
    case TimeInForce::Ioc:
        return "ioc";
    case TimeInForce::Fok:
        return "fok";
    case TimeInForce::Gtc:
        break;
    }
    return "limit";
}

// spot spells side, type and tif in one string; post-only is "-limit-maker"
[[nodiscard]] std::string_view spot_order_type(const OrderCmd& cmd) noexcept {
    const bool buy = cmd.side == Side::Buy;
    if (cmd.order_type == OrderType::Market) {
        return buy ? "buy-market" : "sell-market";
    }
    if (has_flag(cmd.flags, OrderFlags::PostOnly)) {
        return buy ? "buy-limit-maker" : "sell-limit-maker";
    }
    switch (cmd.tif) {
    case TimeInForce::Ioc:
        return buy ? "buy-ioc" : "sell-ioc";
    case TimeInForce::Fok:
        return buy ? "buy-limit-fok" : "sell-limit-fok";
    case TimeInForce::Gtc:
        break;
    }
    return buy ? "buy-limit" : "sell-limit";
}

// swap volume is an integer count of contracts; fractional counts are refused, not rounded
[[nodiscard]] std::optional<std::int64_t> whole_contracts(QtyLots lots, const InstrumentBase& base) noexcept {
    const auto qty = base.lots_to_qty(lots);
    if (qty.is_undef() || qty.raw <= 0 || qty.raw % k_e9 != 0) {
        return std::nullopt;
    }
    return qty.raw / k_e9;
}

} // namespace

std::string_view rest_host() noexcept {
    return ep::k_rest_futures;
}

std::string_view spot_rest_host() noexcept {
    return ep::k_rest_spot;
}

std::string_view rest_host(MarketType market) noexcept {
    switch (market) {
    case MarketType::Spot:
        return ep::k_rest_spot;
    case MarketType::Perp:
    case MarketType::Future:
    case MarketType::Option:
        break;
    }
    return ep::k_rest_futures;
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
    case BuildError::NoSpotAccount:
        return "htx spot account id not loaded";
    case BuildError::ClientOrderIdNotNumeric:
        return "htx swap client order id must fit a positive int64";
    case BuildError::SpotMarketBuyPricedInQuote:
        return "htx spot market buy is priced in quote currency";
    case BuildError::MixedBatch:
        return "cancel batch spans more than one instrument";
    case BuildError::BatchTooLarge:
        return "cancel batch larger than the venue accepts";
    case BuildError::SpotHasNoPositions:
        return "htx spot reports balances, not positions";
    case BuildError::SpotHasNoFunding:
        return "htx spot pays no funding";
    case BuildError::AmendUnsupported:
        return "htx has no modify: cancel and replace";
    case BuildError::OptionsUnsupported:
        return "htx options order entry";
    case BuildError::DatedFuturesUnsupported:
        return "htx dated futures order entry";
    }
    return "unknown";
}

// dated futures and options get their own refusal so the catalogue is not blamed
std::optional<BuildError> market_refusal(MarketType market) noexcept {
    switch (market) {
    case MarketType::Spot:
    case MarketType::Perp:
        return std::nullopt;
    case MarketType::Future:
        return BuildError::DatedFuturesUnsupported;
    case MarketType::Option:
        return BuildError::OptionsUnsupported;
    }
    return BuildError::OptionsUnsupported;
}

std::optional<std::int64_t> swap_client_id(ClientOrderId id) noexcept {
    constexpr auto k_i64_max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (id.raw == 0 || id.raw > k_i64_max) {
        return std::nullopt; // HTX documents the range as [1, 9223372036854775807]
    }
    return static_cast<std::int64_t>(id.raw);
}

net::RestRequest RequestBuilder::contracts() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_swap_contract_info, .body = {}};
}

net::RestRequest RequestBuilder::spot_symbols() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_spot_symbols, .body = {}};
}

net::RestRequest RequestBuilder::server_time() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_server_time, .body = {}};
}

std::string_view SignedRequestBuilder::last_sign_message() const noexcept {
    return {sign_msg_.data(), sign_msg_len_};
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::sign(net::HttpMethod method,
                                                                       MarketType market,
                                                                       std::string_view path,
                                                                       sign::Query& query,
                                                                       std::string_view body,
                                                                       UnixNanos now) {
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NotSigned};
    }
    const auto ts = sign::timestamp(now.ns / 1'000'000'000ULL, timestamp_);
    if (ts.empty()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    query.add(k_param_key, signer_->api_key());
    query.add(k_param_method, k_signature_method);
    query.add(k_param_version, k_signature_version);
    query.add(k_param_timestamp, ts);

    const auto rendered = query.render(query_);
    if (rendered.empty()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }

    const auto host = rest_host(market);
    const auto message = sign::canonical(method_text(method), host, path, rendered, sign_msg_);
    if (message.empty()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    sign_msg_len_ = message.size();

    const auto signature = signer_->sign(message, net::Signer::Encoding::Base64, signature_);
    if (signature.empty()) {
        return std::unexpected{BuildError::NotSigned};
    }
    // base64 has '+', '/' and '=', so the signature is encoded on its own
    const auto encoded = sign::encode(signature, encoded_signature_);
    if (encoded.empty()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }

    Appender target{target_};
    target << path << "?" << rendered << "&Signature=" << encoded;
    if (!target.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }

    net::RestRequest request{};
    request.method = method;
    request.target = target.view();
    request.body = body;
    return request;
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::spot_accounts(UnixNanos now) {
    sign::Query query;
    return sign(net::HttpMethod::Get, MarketType::Spot, ep::k_spot_accounts, query, {}, now);
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
    const bool is_market = cmd.order_type == OrderType::Market;

    net::JsonWriter json{body_};
    Appender path{path_};
    switch (cmd.instrument.market()) {
    case MarketType::Perp: {
        const auto contracts = whole_contracts(cmd.qty, base);
        if (!contracts.has_value()) {
            return std::unexpected{BuildError::BadQuantity};
        }
        const auto client_id = swap_client_id(cmd.client_order_id);
        if (!client_id.has_value()) {
            return std::unexpected{BuildError::ClientOrderIdNotNumeric};
        }
        const auto price_type = swap_order_price_type(cmd);
        json.obj_begin()
            .str("contract_code", base.exchange_symbol.view())
            .str("direction", cmd.side == Side::Sell ? "sell" : "buy")
            .str("offset", k_offset_one_way)
            .num("volume", *contracts)
            .num("lever_rate", k_lever_rate)
            .str("order_price_type", price_type)
            .num("client_order_id", *client_id);
        if (!is_market) {
            const auto price = base.ticks_to_price(cmd.price);
            if (price.is_undef() || price.raw <= 0) {
                return std::unexpected{BuildError::BadPrice};
            }
            json.decimal("price", price);
        }
        if (has_flag(cmd.flags, OrderFlags::ReduceOnly)) {
            json.num("reduce_only", 1);
        }
        json.obj_end();
        path << ep::k_swap_order;
        break;
    }
    case MarketType::Spot: {
        if (spot_account_id_ == 0) {
            return std::unexpected{BuildError::NoSpotAccount};
        }
        if (is_market && cmd.side == Side::Buy) {
            return std::unexpected{BuildError::SpotMarketBuyPricedInQuote};
        }
        const auto amount = base.lots_to_qty(cmd.qty);
        if (amount.is_undef() || amount.raw <= 0) {
            return std::unexpected{BuildError::BadQuantity};
        }
        const auto coid = cmd.exchange_client_order_id.view();
        if (coid.empty()) {
            return std::unexpected{BuildError::NoOrderTarget};
        }
        json.obj_begin()
            .str("account-id", render_i64(spot_account_id_, account_text_))
            .str("symbol", base.exchange_symbol.view())
            .str("type", spot_order_type(cmd))
            .decimal("amount", amount)
            .str("client-order-id", coid);
        if (!is_market) {
            const auto price = base.ticks_to_price(cmd.price);
            if (price.is_undef() || price.raw <= 0) {
                return std::unexpected{BuildError::BadPrice};
            }
            json.decimal("price", price);
        }
        // spot has no reduce-only field
        json.obj_end();
        path << ep::k_spot_order_place;
        break;
    }
    case MarketType::Future:
        return std::unexpected{BuildError::DatedFuturesUnsupported};
    case MarketType::Option:
        return std::unexpected{BuildError::OptionsUnsupported};
    }
    if (!json.ok() || !path.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    sign::Query query;
    return sign(net::HttpMethod::Post, cmd.instrument.market(), path.view(), query, json.view(), now);
}

std::expected<net::RestRequest, BuildError>
SignedRequestBuilder::cancel_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
    if (const auto refusal = market_refusal(cmd.instrument.market()); refusal.has_value()) {
        return std::unexpected{*refusal};
    }
    net::JsonWriter json{body_};
    Appender path{path_};
    switch (cmd.instrument.market()) {
    case MarketType::Perp: {
        const auto symbol = table.exchange_symbol(cmd.instrument);
        if (symbol.empty()) {
            return std::unexpected{BuildError::UnknownInstrument};
        }
        json.obj_begin().str("contract_code", symbol);
        if (!cmd.target_exchange_order_id.empty()) {
            // htx resolves a client_order_id only for 8 hours after the order finishes (err 1061)
            json.str("order_id", cmd.target_exchange_order_id.view());
        } else {
            const auto client_id = swap_client_id(cmd.client_order_id);
            if (!client_id.has_value()) {
                return std::unexpected{BuildError::NoOrderTarget};
            }
            json.str("client_order_id", render_i64(*client_id, account_text_));
        }
        json.obj_end();
        path << ep::k_swap_cancel;
        break;
    }
    case MarketType::Spot: {
        if (!cmd.target_exchange_order_id.empty()) {
            path << ep::k_spot_orders << "/" << cmd.target_exchange_order_id.view() << ep::k_spot_submit_cancel_suffix;
            break;
        }
        const auto coid = cmd.exchange_client_order_id.view();
        if (coid.empty()) {
            return std::unexpected{BuildError::NoOrderTarget};
        }
        json.obj_begin().str("client-order-id", coid).obj_end();
        path << ep::k_spot_cancel_by_client_id;
        break;
    }
    case MarketType::Future:
        return std::unexpected{BuildError::DatedFuturesUnsupported};
    case MarketType::Option:
        return std::unexpected{BuildError::OptionsUnsupported};
    }
    if (!json.ok() || !path.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    sign::Query query;
    return sign(net::HttpMethod::Post, cmd.instrument.market(), path.view(), query, json.view(), now);
}

std::expected<net::RestRequest, BuildError>
SignedRequestBuilder::amend_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
    (void)table;
    (void)now;
    if (const auto refusal = market_refusal(cmd.instrument.market()); refusal.has_value()) {
        return std::unexpected{*refusal};
    }
    return std::unexpected{BuildError::AmendUnsupported};
}

std::expected<net::RestRequest, BuildError>
SignedRequestBuilder::cancel_batch(std::span<const OrderCmd> cmds, const exec::InstrumentTable& table, UnixNanos now) {
    if (cmds.empty()) {
        return std::unexpected{BuildError::NoOrderTarget};
    }
    const auto instrument = cmds.front().instrument;
    for (const auto& cmd : cmds) {
        if (cmd.instrument != instrument) {
            return std::unexpected{BuildError::MixedBatch};
        }
    }

    Appender body{body_};
    Appender path{path_};
    Appender ids{ids_};
    switch (instrument.market()) {
    case MarketType::Perp: {
        if (cmds.size() > k_max_swap_batch_cancel) {
            return std::unexpected{BuildError::BatchTooLarge};
        }
        const auto symbol = table.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{BuildError::UnknownInstrument};
        }
        for (std::size_t i = 0; i < cmds.size(); ++i) {
            const auto client_id = swap_client_id(cmds[i].client_order_id);
            if (!client_id.has_value()) {
                return std::unexpected{BuildError::NoOrderTarget};
            }
            if (i != 0) {
                ids << ",";
            }
            ids << *client_id;
        }
        if (!ids.ok()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        // a comma-separated string; an array is refused
        body << R"({"contract_code":")" << symbol << R"(","client_order_id":")" << ids.view() << R"("})";
        path << ep::k_swap_cancel;
        break;
    }
    case MarketType::Spot: {
        if (cmds.size() > k_max_spot_batch_cancel) {
            return std::unexpected{BuildError::BatchTooLarge};
        }
        body << R"({"client-order-ids":[)";
        for (std::size_t i = 0; i < cmds.size(); ++i) {
            const auto coid = cmds[i].exchange_client_order_id.view();
            if (coid.empty()) {
                return std::unexpected{BuildError::NoOrderTarget};
            }
            body << (i == 0 ? "\"" : ",\"") << coid << "\"";
        }
        body << "]}";
        path << ep::k_spot_batch_cancel;
        break;
    }
    case MarketType::Future:
        return std::unexpected{BuildError::DatedFuturesUnsupported};
    case MarketType::Option:
        return std::unexpected{BuildError::OptionsUnsupported};
    }
    if (!body.ok() || !path.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    sign::Query query;
    return sign(net::HttpMethod::Post, instrument.market(), path.view(), query, body.view(), now);
}

std::expected<net::RestRequest, BuildError>
SignedRequestBuilder::open_orders(MarketType market, std::uint32_t page, UnixNanos now) {
    net::JsonWriter json{body_};
    Appender path{path_};
    sign::Query query;
    auto method = net::HttpMethod::Post;
    switch (market) {
    case MarketType::Perp:
        // omitting contract_code makes this account-wide
        json.obj_begin()
            .num("page_index", static_cast<std::int64_t>(page == 0 ? 1 : page))
            .num("page_size", static_cast<std::int64_t>(k_swap_page_size))
            .obj_end();
        path << ep::k_swap_open_orders;
        break;
    case MarketType::Spot:
        method = net::HttpMethod::Get;
        // omitting account-id and symbol gives the account-wide read
        query.add("size", k_spot_page_limit);
        path << ep::k_spot_open_orders;
        break;
    case MarketType::Future:
        return std::unexpected{BuildError::DatedFuturesUnsupported};
    case MarketType::Option:
        return std::unexpected{BuildError::OptionsUnsupported};
    }
    if (!json.ok() || !path.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(
        method, market, path.view(), query, method == net::HttpMethod::Post ? json.view() : std::string_view{}, now);
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::query_order(const ExchangeClientOrderId& id,
                                                                              std::int64_t numeric_id,
                                                                              std::string_view contract_code,
                                                                              MarketType market,
                                                                              UnixNanos now) {
    net::JsonWriter json{body_};
    Appender path{path_};
    sign::Query query;
    auto method = net::HttpMethod::Post;
    switch (market) {
    case MarketType::Perp: {
        if (numeric_id <= 0 || contract_code.empty()) {
            return std::unexpected{BuildError::NoOrderTarget};
        }
        json.obj_begin()
            .str("contract_code", contract_code)
            .str("client_order_id", render_i64(numeric_id, account_text_))
            .obj_end();
        path << ep::k_swap_order_info;
        break;
    }
    case MarketType::Spot: {
        if (id.empty()) {
            return std::unexpected{BuildError::NoOrderTarget};
        }
        method = net::HttpMethod::Get;
        // camelCase here and hyphenated in bodies; htx's own spelling
        query.add("clientOrderId", id.view());
        path << ep::k_spot_order_by_client_id;
        break;
    }
    case MarketType::Future:
        return std::unexpected{BuildError::DatedFuturesUnsupported};
    case MarketType::Option:
        return std::unexpected{BuildError::OptionsUnsupported};
    }
    if (!json.ok() || !path.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(
        method, market, path.view(), query, method == net::HttpMethod::Post ? json.view() : std::string_view{}, now);
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::positions(MarketType market, UnixNanos now) {
    net::JsonWriter json{body_};
    Appender path{path_};
    switch (market) {
    case MarketType::Perp:
        // contract_code omitted: the whole cross account in one call.
        json.obj_begin().obj_end();
        path << ep::k_swap_position_info;
        break;
    case MarketType::Spot:
        return std::unexpected{BuildError::SpotHasNoPositions};
    case MarketType::Future:
        return std::unexpected{BuildError::DatedFuturesUnsupported};
    case MarketType::Option:
        return std::unexpected{BuildError::OptionsUnsupported};
    }
    if (!json.ok() || !path.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    sign::Query query;
    return sign(net::HttpMethod::Post, market, path.view(), query, json.view(), now);
}

std::expected<net::RestRequest, BuildError>
SignedRequestBuilder::financial_record(MarketType market, UnixNanos since, UnixNanos now) {
    switch (market) {
    case MarketType::Perp:
        break;
    case MarketType::Spot:
        return std::unexpected{BuildError::SpotHasNoFunding};
    case MarketType::Future:
        return std::unexpected{BuildError::DatedFuturesUnsupported};
    case MarketType::Option:
        return std::unexpected{BuildError::OptionsUnsupported};
    }

    const auto now_ms = now.ns / 1'000'000ULL;
    const auto floor_ms = now_ms > k_financial_record_window_ms ? now_ms - k_financial_record_window_ms : 0;
    const auto since_ms = since.ns / 1'000'000ULL;

    net::JsonWriter json{body_};
    // contract_code omitted: the whole margin account in one call
    json.obj_begin()
        .str("mar_acct", k_cross_margin_account)
        .str("type", k_funding_record_types)
        .num("start_time", static_cast<std::int64_t>(since_ms > floor_ms ? since_ms : floor_ms))
        .obj_end();
    if (!json.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    Appender path{path_};
    path << ep::k_swap_financial_record;
    if (!path.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    sign::Query query;
    return sign(net::HttpMethod::Post, market, path.view(), query, json.view(), now);
}

} // namespace raikiri::venues::htx
