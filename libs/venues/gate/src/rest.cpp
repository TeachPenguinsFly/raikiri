#include <raikiri/net/json_writer.hpp>
#include <raikiri/venues/gate/endpoints.hpp>
#include <raikiri/venues/gate/rest.hpp>

#include <charconv>
#include <cstring>
#include <optional>
#include <span>

namespace raikiri::venues::gate {

namespace {

constexpr std::string_view k_hdr_key = "KEY";
constexpr std::string_view k_hdr_sign = "SIGN";
constexpr std::string_view k_hdr_timestamp = "Timestamp";

// Gate's own spelling of the spot account type. A UNIFIED account refuses
// "spot" and wants "unified"; there is no VenueConfig field for that yet, so a
// unified-account key will see every spot order refused rather than routed to
// the wrong sub-account.
constexpr std::string_view k_spot_account = "spot";

// Both list endpoints cap at 1000 and default to 100; reconciliation wants the
// whole picture, not a paginated approximation of it.
// Gate caps these listings at 100 and answers INVALID_PARAM_VALUE above it —
// measured against the live futures positions endpoint, which refused 1000 and
// accepted 100. The orders and fills listings are the same family; they have no
// caller yet and would have failed the same way the day they got one.
constexpr std::string_view k_page_limit = "100";

// Fills the builder's target buffer. A part that would overflow is dropped
// rather than truncated mid-token, so an over-long contract name yields a
// request Gate refuses with CONTRACT_NOT_FOUND instead of one that names a
// different contract.
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

// Gate spells post-only as a TIME IN FORCE (`poc`), not as a separate flag.
// Sending post_only alongside tif=gtc is ignored and the order crosses.
[[nodiscard]] std::string_view tif_text(TimeInForce tif, OrderFlags flags) noexcept {
    if (has_flag(flags, OrderFlags::PostOnly)) {
        return "poc";
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

// Whole contracts from lots. Gate's futures `size` is a JSON integer count of
// contracts, so a quantity that is not a whole number of them cannot be sent —
// and must not be rounded into one that can.
[[nodiscard]] std::optional<std::int64_t> whole_contracts(QtyLots lots,
                                                          const InstrumentBase& base) noexcept {
    const auto qty = base.lots_to_qty(lots);
    if (qty.is_undef() || qty.raw <= 0 || qty.raw % k_e9 != 0) {
        return std::nullopt;
    }
    return qty.raw / k_e9;
}

[[nodiscard]] std::string_view order_target(const OrderCmd& cmd, std::span<char> out) noexcept {
    if (!cmd.target_exchange_order_id.empty()) {
        return cmd.target_exchange_order_id.view();
    }
    return client_id_text(cmd.exchange_client_order_id, out);
}

} // namespace

std::string_view rest_host(bool testnet) noexcept {
    return ep::rest_host(testnet);
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
    case BuildError::SpotMarketBuyNotExpressible:
        return "gate spot market buy is priced in quote currency";
    case BuildError::OptionsUnsupported:
        return "gate options order entry";
    case BuildError::DatedFuturesUnsupported:
        return "gate delivery (dated futures) order entry";
    }
    return "unknown";
}

std::string_view client_id_text(const ExchangeClientOrderId& id, std::span<char> out) noexcept {
    const auto body = id.view();
    if (body.empty() || body.size() > k_client_id_body_max ||
        out.size() < k_client_id_prefix.size() + body.size()) {
        return {};
    }
    std::memcpy(out.data(), k_client_id_prefix.data(), k_client_id_prefix.size());
    std::memcpy(out.data() + k_client_id_prefix.size(), body.data(), body.size());
    return {out.data(), k_client_id_prefix.size() + body.size()};
}

std::string_view client_id_from_text(std::string_view text) noexcept {
    if (!text.starts_with(k_client_id_prefix)) {
        return {};
    }
    return text.substr(k_client_id_prefix.size());
}

// Every builder below writes into the same buffer, so a request must be sent
// before the next one is built. One catalogue load at a time is the only use.
net::RestRequest RequestBuilder::contracts(std::string_view settle) noexcept {
    TargetWriter writer{target_};
    writer.put(ep::k_futures_prefix);
    writer.put(settle);
    writer.put(ep::k_futures_contracts);
    return writer.get();
}

net::RestRequest RequestBuilder::currency_pairs() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_spot_currency_pairs, .body = {}};
}

net::RestRequest RequestBuilder::server_time() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_server_time, .body = {}};
}

net::RestRequest RequestBuilder::order_book(std::string_view settle,
                                            std::string_view contract,
                                            std::uint32_t depth) noexcept {
    TargetWriter writer{target_};
    writer.put(ep::k_futures_prefix);
    writer.put(settle);
    writer.put(ep::k_futures_order_book);
    writer.put("?contract=");
    writer.put(contract);
    writer.put("&limit=");
    writer.put(depth);
    // with_id is what makes this a seed rather than a picture: without it the
    // body has no `id` and there is no way to say where in the update chain
    // these levels belong. Verified live — the field is absent by default.
    writer.put("&with_id=true");
    return writer.get();
}

// -------------------------------------------------------------- signed ----

std::string_view SignedRequestBuilder::last_sign_message() const noexcept {
    return {sign_msg_.data(), sign_msg_len_};
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::sign(net::HttpMethod method,
                                                                       std::string_view path,
                                                                       std::string_view query,
                                                                       std::string_view body,
                                                                       UnixNanos now) {
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NotSigned};
    }
    Appender ts{timestamp_};
    ts << static_cast<std::int64_t>(now.ns / 1'000'000'000ULL);
    if (!ts.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }

    // A GET signs SHA512 of the EMPTY string, not an empty field: the term is
    // always present and always 128 hex characters.
    const auto body_hash = net::sha512_hex(body, body_hash_);
    if (body_hash.empty()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }

    Appender msg{sign_msg_};
    msg << method_text(method) << "\n"
        << path << "\n"
        << query << "\n"
        << body_hash << "\n"
        << ts.view();
    if (!msg.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    sign_msg_len_ = msg.view().size();

    const auto signature = signer_->sign(msg.view(), net::Signer::Encoding::Hex512, signature_);
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
    request.headers = {
        {k_hdr_key, signer_->api_key()}, {k_hdr_timestamp, ts.view()}, {k_hdr_sign, signature}};
    return request;
}

// Gate serves spot and perpetuals here; dated futures live under /delivery and
// options under /options, each with its own model. Callers ask BEFORE reading
// the instrument table, which resolves an id by its whole value and would
// otherwise answer UnknownInstrument — blaming the catalogue for a market this
// adapter never served.
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
    std::array<char, k_client_id_bytes> coid_buf{};
    const auto coid = client_id_text(cmd.exchange_client_order_id, coid_buf);
    if (coid.empty()) {
        return std::unexpected{BuildError::NoOrderTarget};
    }
    const auto market = cmd.instrument.market();
    const bool is_market = cmd.order_type == OrderType::Market;

    net::JsonWriter json{body_};
    Appender path{path_};
    switch (market) {
    case MarketType::Perp: {
        const auto contracts = whole_contracts(cmd.qty, base);
        if (!contracts.has_value()) {
            return std::unexpected{BuildError::BadQuantity};
        }
        json.obj_begin()
            .str("contract", base.exchange_symbol.view())
            // The SIDE lives in the sign of `size`: positive bids, negative
            // asks. There is no side field to disagree with it.
            .num("size", cmd.side == Side::Sell ? -*contracts : *contracts);
        if (is_market) {
            // Gate has no market order type on futures: price "0" with tif ioc
            // IS the market order, and any other tif is refused.
            json.str("price", "0").str("tif", "ioc");
        } else {
            const auto price = base.ticks_to_price(cmd.price);
            if (price.is_undef() || price.raw <= 0) {
                return std::unexpected{BuildError::BadPrice};
            }
            json.decimal("price", price).str("tif", tif_text(cmd.tif, cmd.flags));
        }
        json.str("text", coid);
        if (has_flag(cmd.flags, OrderFlags::ReduceOnly)) {
            json.boolean("reduce_only", true);
        }
        json.obj_end();
        path << ep::k_futures_prefix << k_settle_usdt << ep::k_futures_orders;
        break;
    }
    case MarketType::Spot: {
        if (is_market && cmd.side == Side::Buy) {
            return std::unexpected{BuildError::SpotMarketBuyNotExpressible};
        }
        const auto amount = base.lots_to_qty(cmd.qty);
        if (amount.is_undef() || amount.raw <= 0) {
            return std::unexpected{BuildError::BadQuantity};
        }
        json.obj_begin()
            .str("text", coid)
            .str("currency_pair", base.exchange_symbol.view())
            .str("type", is_market ? "market" : "limit")
            .str("account", k_spot_account)
            .str("side", cmd.side == Side::Sell ? "sell" : "buy")
            .decimal("amount", amount);
        if (is_market) {
            // A market order carries no price, and Gate accepts only ioc/fok
            // for one; gtc on a market order is refused outright.
            json.str("time_in_force", cmd.tif == TimeInForce::Fok ? "fok" : "ioc");
        } else {
            const auto price = base.ticks_to_price(cmd.price);
            if (price.is_undef() || price.raw <= 0) {
                return std::unexpected{BuildError::BadPrice};
            }
            // ReduceOnly is deliberately not forwarded: a spot balance cannot
            // be short, so Gate has no field for it.
            json.decimal("price", price).str("time_in_force", tif_text(cmd.tif, cmd.flags));
        }
        json.obj_end();
        path << ep::k_spot_orders;
        break;
    }
    case MarketType::Future:
        return std::unexpected{BuildError::DatedFuturesUnsupported};
    case MarketType::Option:
        // Gate lists options under /api/v4/options with its own underlying and
        // expiry model; nothing here maps an OptionInstrument to it yet.
        return std::unexpected{BuildError::OptionsUnsupported};
    }
    if (!json.ok() || !path.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Post, path.view(), {}, json.view(), now);
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::cancel_order(
    const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
    if (const auto refusal = market_refusal(cmd.instrument.market()); refusal.has_value()) {
        return std::unexpected{*refusal};
    }
    std::array<char, k_client_id_bytes> coid_buf{};
    const auto target = order_target(cmd, coid_buf);
    if (target.empty()) {
        return std::unexpected{BuildError::NoOrderTarget};
    }

    Appender path{path_};
    Appender query{query_};
    switch (cmd.instrument.market()) {
    case MarketType::Perp:
        // A `t-` id resolves only while the order is in the book, and for 60
        // seconds after it finishes. Past that Gate answers ORDER_NOT_FOUND
        // for an order that very much existed, which is why a cancel prefers
        // the venue id whenever the order store has one.
        path << ep::k_futures_prefix << k_settle_usdt << ep::k_futures_orders << "/" << target;
        break;
    case MarketType::Spot: {
        const auto symbol = table.exchange_symbol(cmd.instrument);
        if (symbol.empty()) {
            return std::unexpected{BuildError::UnknownInstrument};
        }
        path << ep::k_spot_orders << "/" << target;
        query << "currency_pair=" << symbol << "&account=" << k_spot_account;
        break;
    }
    case MarketType::Future:
        return std::unexpected{BuildError::DatedFuturesUnsupported};
    case MarketType::Option:
        return std::unexpected{BuildError::OptionsUnsupported};
    }
    if (!path.ok() || !query.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Delete, path.view(), query.view(), {}, now);
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
    std::array<char, k_client_id_bytes> coid_buf{};
    const auto target = order_target(cmd, coid_buf);
    if (target.empty()) {
        return std::unexpected{BuildError::NoOrderTarget};
    }

    net::JsonWriter json{body_};
    Appender path{path_};
    Appender query{query_};
    auto method = net::HttpMethod::Put;
    switch (cmd.instrument.market()) {
    case MarketType::Perp: {
        json.obj_begin();
        if (!cmd.qty.is_undef()) {
            const auto contracts = whole_contracts(cmd.qty, base);
            if (!contracts.has_value()) {
                return std::unexpected{BuildError::BadQuantity};
            }
            // An amend re-states the SIGNED size, so a resize that forgot the
            // sign would flip a short into a long.
            json.num("size", cmd.side == Side::Sell ? -*contracts : *contracts);
        }
        if (!cmd.price.is_undef()) {
            json.decimal("price", base.ticks_to_price(cmd.price));
        }
        json.obj_end();
        path << ep::k_futures_prefix << k_settle_usdt << ep::k_futures_orders << "/" << target;
        break;
    }
    case MarketType::Spot: {
        // The one PATCH here: Gate offers no PUT for a spot amend.
        method = net::HttpMethod::Patch;
        json.obj_begin();
        if (!cmd.qty.is_undef()) {
            json.decimal("amount", base.lots_to_qty(cmd.qty));
        }
        if (!cmd.price.is_undef()) {
            json.decimal("price", base.ticks_to_price(cmd.price));
        }
        json.obj_end();
        path << ep::k_spot_orders << "/" << target;
        query << "currency_pair=" << base.exchange_symbol.view() << "&account=" << k_spot_account;
        break;
    }
    case MarketType::Future:
        return std::unexpected{BuildError::DatedFuturesUnsupported};
    case MarketType::Option:
        return std::unexpected{BuildError::OptionsUnsupported};
    }
    if (!json.ok() || !path.ok() || !query.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(method, path.view(), query.view(), json.view(), now);
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::cancel_batch(
    std::span<const OrderCmd> cmds, const exec::InstrumentTable& table, UnixNanos now) {
    if (cmds.empty() || cmds.size() > k_max_batch_cancel) {
        return std::unexpected{BuildError::NoOrderTarget};
    }
    const auto market = cmds.front().instrument.market();

    // A batch that straddles markets would go to one endpoint and silently
    // lose the other market's cancels.
    for (const auto& cmd : cmds) {
        if (cmd.instrument.market() != market) {
            return std::unexpected{BuildError::UnknownInstrument};
        }
    }

    Appender body{body_};
    Appender path{path_};
    switch (market) {
    case MarketType::Perp:
        body << "[";
        for (std::size_t i = 0; i < cmds.size(); ++i) {
            std::array<char, k_client_id_bytes> coid_buf{};
            const auto target = order_target(cmds[i], coid_buf);
            if (target.empty()) {
                return std::unexpected{BuildError::NoOrderTarget};
            }
            body << (i == 0 ? "\"" : ",\"") << target << "\"";
        }
        body << "]";
        path << ep::k_futures_prefix << k_settle_usdt << ep::k_futures_batch_cancel;
        break;
    case MarketType::Spot:
        body << "[";
        for (std::size_t i = 0; i < cmds.size(); ++i) {
            const auto symbol = table.exchange_symbol(cmds[i].instrument);
            if (symbol.empty()) {
                return std::unexpected{BuildError::UnknownInstrument};
            }
            std::array<char, k_client_id_bytes> coid_buf{};
            const auto target = order_target(cmds[i], coid_buf);
            if (target.empty()) {
                return std::unexpected{BuildError::NoOrderTarget};
            }
            body << (i == 0 ? "{" : ",{") << R"("currency_pair":")" << symbol << R"(","id":")"
                 << target << "\"}";
        }
        body << "]";
        path << ep::k_spot_cancel_batch;
        break;
    case MarketType::Future:
        return std::unexpected{BuildError::DatedFuturesUnsupported};
    case MarketType::Option:
        return std::unexpected{BuildError::OptionsUnsupported};
    }
    if (!body.ok() || !path.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Post, path.view(), {}, body.view(), now);
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::open_orders(MarketType market,
                                                                              UnixNanos now) {
    Appender path{path_};
    Appender query{query_};
    switch (market) {
    case MarketType::Perp:
        path << ep::k_futures_prefix << k_settle_usdt << ep::k_futures_orders;
        query << "status=open&limit=" << k_page_limit;
        break;
    case MarketType::Spot:
        // /spot/open_orders answers with one entry PER CURRENCY PAIR, each
        // holding its own `orders` array; `limit` counts pairs, not orders.
        path << ep::k_spot_open_orders;
        query << "limit=100&account=" << k_spot_account;
        break;
    case MarketType::Future:
        return std::unexpected{BuildError::DatedFuturesUnsupported};
    case MarketType::Option:
        return std::unexpected{BuildError::OptionsUnsupported};
    }
    if (!path.ok() || !query.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Get, path.view(), query.view(), {}, now);
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::query_order(
    const ExchangeClientOrderId& id, MarketType market, UnixNanos now) {
    std::array<char, k_client_id_bytes> coid_buf{};
    const auto target = client_id_text(id, coid_buf);
    if (target.empty()) {
        return std::unexpected{BuildError::NoOrderTarget};
    }

    Appender path{path_};
    switch (market) {
    case MarketType::Perp:
        path << ep::k_futures_prefix << k_settle_usdt << ep::k_futures_orders << "/" << target;
        break;
    case MarketType::Spot:
        // /spot/orders/{id} demands currency_pair, and a client order id does
        // not carry one. The caller answers from the open-order sweep instead.
        return std::unexpected{BuildError::UnknownInstrument};
    case MarketType::Future:
        return std::unexpected{BuildError::DatedFuturesUnsupported};
    case MarketType::Option:
        return std::unexpected{BuildError::OptionsUnsupported};
    }
    if (!path.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Get, path.view(), {}, {}, now);
}

std::expected<net::RestRequest, BuildError>
SignedRequestBuilder::my_trades(MarketType market, UnixNanos since, UnixNanos now) {
    // SECONDS, unlike the millisecond stamps the same responses carry back.
    const auto from_s = static_cast<std::int64_t>(since.ns / 1'000'000'000ULL);

    Appender path{path_};
    Appender query{query_};
    switch (market) {
    case MarketType::Perp:
        // The plain /my_trades pages by order id and cannot answer "since";
        // only the _timerange variant takes from/to.
        path << ep::k_futures_prefix << k_settle_usdt << ep::k_futures_my_trades;
        query << "limit=" << k_page_limit;
        if (from_s > 0) {
            query << "&from=" << from_s;
        }
        break;
    case MarketType::Spot:
        path << ep::k_spot_my_trades;
        query << "limit=" << k_page_limit << "&account=" << k_spot_account;
        if (from_s > 0) {
            query << "&from=" << from_s;
        }
        break;
    case MarketType::Future:
        return std::unexpected{BuildError::DatedFuturesUnsupported};
    case MarketType::Option:
        return std::unexpected{BuildError::OptionsUnsupported};
    }
    if (!path.ok() || !query.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Get, path.view(), query.view(), {}, now);
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::positions(MarketType market,
                                                                            UnixNanos now) {
    Appender path{path_};
    Appender query{query_};
    switch (market) {
    case MarketType::Perp:
        path << ep::k_futures_prefix << k_settle_usdt << ep::k_futures_positions;
        // holding=true drops the size-0 rows Gate keeps for every contract the
        // account ever touched; absent then means flat, as PositionReport reads.
        query << "holding=true&limit=" << k_page_limit;
        break;
    case MarketType::Spot:
        // A spot balance is not a position, and reporting one as flat would be
        // a claim about a concept Gate does not have here.
        return std::unexpected{BuildError::UnknownInstrument};
    case MarketType::Future:
        return std::unexpected{BuildError::DatedFuturesUnsupported};
    case MarketType::Option:
        return std::unexpected{BuildError::OptionsUnsupported};
    }
    if (!path.ok() || !query.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Get, path.view(), query.view(), {}, now);
}

// The account book: settlements AND commissions.
//
// `change` is SIGNED the way the account moved, which is already the domain's
// convention. `from` is in SECONDS — gate states every timestamp in this ledger
// in seconds, unlike the millisecond stamps its order endpoints use, and
// passing milliseconds here silently asks for a window fifty thousand years
// wide, which the venue answers by returning nothing.
std::expected<net::RestRequest, BuildError>
SignedRequestBuilder::funding_book(MarketType market, UnixNanos since, UnixNanos now) {
    if (market != MarketType::Perp) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    Appender path{path_};
    Appender query{query_};
    path << ep::k_futures_prefix << k_settle_usdt << ep::k_futures_account_book;
    // NO type FILTER. `fund` and `fee` share this endpoint, and the fee rows
    // are what a fee reconciliation compares against; asking twice would pay
    // for one answer twice.
    query << "limit=" << k_page_limit;
    if (since.ns != 0) {
        char buf[24];
        const auto [end, ec] = std::to_chars(buf, buf + sizeof buf, since.ns / 1'000'000'000);
        if (ec == std::errc{}) {
            query << "&from=" << std::string_view{buf, static_cast<std::size_t>(end - buf)};
        }
    }
    if (!path.ok() || !query.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Get, path.view(), query.view(), {}, now);
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::futures_account(UnixNanos now) {
    Appender path{path_};
    path << ep::k_futures_prefix << k_settle_usdt << ep::k_futures_accounts;
    if (!path.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Get, path.view(), {}, {}, now);
}

} // namespace raikiri::venues::gate
