#include <raikiri/venues/bingx/client_id.hpp>
#include <raikiri/venues/bingx/endpoints.hpp>
#include <raikiri/venues/bingx/rest.hpp>

#include <charconv>
#include <cstring>
#include <optional>
#include <span>

namespace raikiri::venues::bingx {

namespace {

constexpr std::string_view k_hdr_api_key = "X-BX-APIKEY";

// bingx's maximum and default; signed like any other parameter
constexpr std::string_view k_recv_window = "5000";

// backdated half a second so forward drift stays inside recvWindow
constexpr std::int64_t k_clock_backdate_ms = 500;

// bingx allows up to 1000
constexpr std::string_view k_income_limit = "200";

// drops a part that would overflow rather than truncating a token
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

// the canonical string is both hashed and sent; overflow and unsafe values are sticky
class Query {
public:
    explicit Query(std::span<char> buf) noexcept : buf_{buf} {}

    Query& put(std::string_view key, std::string_view value) noexcept {
        if (bad_ || !safe(value)) {
            bad_ = true;
            return *this;
        }
        const auto need = (len_ == 0 ? 0U : 1U) + key.size() + 1 + value.size();
        if (len_ + need > buf_.size()) {
            bad_ = true;
            return *this;
        }
        if (len_ != 0) {
            buf_[len_++] = '&';
        }
        std::memcpy(buf_.data() + len_, key.data(), key.size());
        len_ += key.size();
        buf_[len_++] = '=';
        std::memcpy(buf_.data() + len_, value.data(), value.size());
        len_ += value.size();
        return *this;
    }

    Query& put(std::string_view key, std::int64_t value) noexcept {
        std::array<char, 24> num{};
        const auto [stop, ec] = std::to_chars(num.data(), num.data() + num.size(), value);
        if (ec != std::errc{}) {
            bad_ = true;
            return *this;
        }
        return put(key, std::string_view{num.data(), static_cast<std::size_t>(stop - num.data())});
    }

    // straight from fixed point, with no double round trip
    Query& put(std::string_view key, FixedE9 value) noexcept {
        std::array<char, 32> num{};
        const auto text = format_e9(value, num.data(), num.size());
        if (text.empty()) {
            bad_ = true;
            return *this;
        }
        return put(key, text);
    }

    [[nodiscard]] bool ok() const noexcept { return !bad_; }

    [[nodiscard]] std::string_view view() const noexcept {
        return bad_ ? std::string_view{} : std::string_view{buf_.data(), len_};
    }

private:
    // a separator in a value would become a second signed parameter
    [[nodiscard]] static bool safe(std::string_view value) noexcept {
        for (const char c : value) {
            if (c == '&' || c == '=' || c == '?' || c == '#' || c == '%' || c == '+' || c == ' ' ||
                c == '\r' || c == '\n' || c == '[' || c == '{') {
                return false;
            }
        }
        return true;
    }

    std::span<char> buf_;
    std::size_t len_{0};
    bool bad_{false};
};

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

} // namespace

std::string_view rest_host() noexcept {
    return ep::k_rest;
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
    case BuildError::UnsafeParameter:
        return "parameter value would split the signed query";
    case BuildError::AmendPriceUnsupported:
        return "bingx amends quantity only";
    case BuildError::SpotAmendUnsupported:
        return "bingx spot amend (only cancelReplace, which mints a new order)";
    case BuildError::SpotPositionsUnsupported:
        return "bingx spot position pull";
    case BuildError::OptionsUnsupported:
        return "bingx options order entry";
    case BuildError::DatedFuturesUnsupported:
        return "bingx dated futures order entry";
    }
    return "unknown";
}

std::optional<BuildError> market_refusal(MarketType market) noexcept {
    switch (market) {
    case MarketType::Spot:
    case MarketType::Perp:
        return std::nullopt;
    case MarketType::Future:
        // bingx lists no dated futures
        return BuildError::DatedFuturesUnsupported;
    case MarketType::Option:
        return BuildError::OptionsUnsupported;
    }
    return BuildError::OptionsUnsupported;
}

std::string_view time_in_force(TimeInForce tif, OrderFlags flags) noexcept {
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

std::string_view position_side(Side side, OrderFlags flags, bool hedge) noexcept {
    if (!hedge) {
        return "BOTH"; // one-way mode; long/short is refused with 109400
    }
    // a buy opens long and closes short
    const bool reduce = has_flag(flags, OrderFlags::ReduceOnly);
    return (side == Side::Buy) != reduce ? "LONG" : "SHORT";
}

net::RestRequest RequestBuilder::contracts() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_perp_contracts, .body = {}};
}

net::RestRequest RequestBuilder::spot_symbols() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_spot_symbols, .body = {}};
}

net::RestRequest RequestBuilder::server_time() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_server_time, .body = {}};
}

// builders share one buffer; send before building the next
net::RestRequest RequestBuilder::premium_index(std::string_view symbol) noexcept {
    TargetWriter writer{target_};
    writer.put(ep::k_premium_index);
    writer.put("?symbol=");
    writer.put(symbol);
    return writer.get();
}

net::RestRequest RequestBuilder::depth(std::string_view symbol, std::uint32_t limit) noexcept {
    TargetWriter writer{target_};
    writer.put(ep::k_depth);
    writer.put("?symbol=");
    writer.put(symbol);
    writer.put("&limit=");
    writer.put(limit);
    return writer.get();
}

net::RestRequest RequestBuilder::ticker(std::string_view symbol) noexcept {
    TargetWriter writer{target_};
    writer.put(ep::k_ticker);
    writer.put("?symbol=");
    writer.put(symbol);
    return writer.get();
}

std::string_view SignedRequestBuilder::last_sign_message() const noexcept {
    return {query_.data(), canonical_len_};
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::sign(net::HttpMethod method,
                                                                       std::string_view path,
                                                                       std::string_view canonical) {
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NotSigned};
    }
    if (canonical.empty()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    canonical_len_ = canonical.size();

    const auto signature = signer_->sign(canonical, net::Signer::Encoding::Hex, signature_);
    if (signature.empty()) {
        return std::unexpected{BuildError::NotSigned};
    }

    // the sent query is copied from the hashed buffer; do not rebuild it
    std::size_t len = 0;
    const auto append = [&](std::string_view text) {
        if (len + text.size() > target_.size()) {
            len = target_.size() + 1; // sticky overflow
            return;
        }
        std::memcpy(target_.data() + len, text.data(), text.size());
        len += text.size();
    };
    append(path);
    append("?");
    append(canonical);
    append("&signature=");
    append(signature);
    if (len > target_.size()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }

    net::RestRequest request{};
    request.method = method;
    request.target = std::string_view{target_.data(), len};
    request.body = {}; // bingx carries post and delete parameters in the query
    request.headers = {{k_hdr_api_key, signer_->api_key()}};
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
    const auto coid = client_id_text(cmd.exchange_client_order_id);
    if (coid.empty()) {
        return std::unexpected{BuildError::NoOrderTarget};
    }
    const auto qty = order_qty(cmd, base);
    if (!qty.has_value()) {
        return std::unexpected{BuildError::BadQuantity};
    }
    const bool is_market = cmd.order_type == OrderType::Market;
    const auto price = is_market ? std::nullopt : order_price(cmd, base);
    if (!is_market && !price.has_value()) {
        return std::unexpected{BuildError::BadPrice};
    }
    const auto ts = static_cast<std::int64_t>(now.ns / 1'000'000ULL) - k_clock_backdate_ms;

    Query query{query_};
    switch (cmd.instrument.market()) {
    case MarketType::Perp: {
        // bingx's docs spell this both ways; send both, an unknown parameter is ignored
        query.put("clientOrderID", coid).put("clientOrderId", coid);
        query.put("positionSide", position_side(cmd.side, cmd.flags, hedge_));
        if (price.has_value()) {
            query.put("price", *price);
        }
        query.put("quantity", *qty).put("recvWindow", k_recv_window);
        // reduceOnly is one-way only; hedge mode must not send it
        if (!hedge_ && has_flag(cmd.flags, OrderFlags::ReduceOnly)) {
            query.put("reduceOnly", "true");
        }
        query.put("side", side_name(cmd.side))
            .put("symbol", base.exchange_symbol.view())
            .put("timeInForce", time_in_force(cmd.tif, cmd.flags))
            .put("timestamp", ts)
            .put("type", is_market ? "MARKET" : "LIMIT");
        if (!query.ok()) {
            return std::unexpected{BuildError::UnsafeParameter};
        }
        return sign(net::HttpMethod::Post, ep::k_perp_order, query.view());
    }
    case MarketType::Spot: {
        // spot spells it newClientOrderId; a market buy takes a base quantity
        query.put("newClientOrderId", coid);
        if (price.has_value()) {
            query.put("price", *price);
        }
        query.put("quantity", *qty)
            .put("recvWindow", k_recv_window)
            .put("side", side_name(cmd.side))
            .put("symbol", base.exchange_symbol.view())
            .put("timeInForce", time_in_force(cmd.tif, cmd.flags))
            .put("timestamp", ts)
            .put("type", is_market ? "MARKET" : "LIMIT");
        // spot has no reduceOnly
        if (!query.ok()) {
            return std::unexpected{BuildError::UnsafeParameter};
        }
        return sign(net::HttpMethod::Post, ep::k_spot_order, query.view());
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
    const auto* instrument = table.find(cmd.instrument);
    if (instrument == nullptr) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    const auto& base = base_of(*instrument);
    const auto venue_id = cmd.target_exchange_order_id.view();
    const bool by_venue_id = all_digits(venue_id);
    const auto coid = client_id_text(cmd.exchange_client_order_id);
    if (!by_venue_id && coid.empty()) {
        return std::unexpected{BuildError::NoOrderTarget};
    }
    const auto ts = static_cast<std::int64_t>(now.ns / 1'000'000ULL) - k_clock_backdate_ms;

    Query query{query_};
    switch (cmd.instrument.market()) {
    case MarketType::Perp: {
        if (by_venue_id) {
            query.put("orderId", venue_id);
        } else {
            query.put("clientOrderID", coid).put("clientOrderId", coid);
        }
        query.put("recvWindow", k_recv_window)
            .put("symbol", base.exchange_symbol.view())
            .put("timestamp", ts);
        if (!query.ok()) {
            return std::unexpected{BuildError::UnsafeParameter};
        }
        return sign(net::HttpMethod::Delete, ep::k_perp_order, query.view());
    }
    case MarketType::Spot: {
        // spot cancels by post and spells the id clientOrderID
        if (by_venue_id) {
            query.put("orderId", venue_id);
        } else {
            query.put("clientOrderID", coid);
        }
        query.put("recvWindow", k_recv_window)
            .put("symbol", base.exchange_symbol.view())
            .put("timestamp", ts);
        if (!query.ok()) {
            return std::unexpected{BuildError::UnsafeParameter};
        }
        return sign(net::HttpMethod::Post, ep::k_spot_cancel, query.view());
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
    if (cmd.instrument.market() == MarketType::Spot) {
        return std::unexpected{BuildError::SpotAmendUnsupported};
    }
    // amend takes no price, so one that restates a price is refused
    if (!cmd.price.is_undef() && cmd.price.raw != 0) {
        return std::unexpected{BuildError::AmendPriceUnsupported};
    }
    const auto* instrument = table.find(cmd.instrument);
    if (instrument == nullptr) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    const auto& base = base_of(*instrument);
    const auto qty = order_qty(cmd, base);
    if (!qty.has_value()) {
        return std::unexpected{BuildError::BadQuantity};
    }
    const auto venue_id = cmd.target_exchange_order_id.view();
    const bool by_venue_id = all_digits(venue_id);
    const auto coid = client_id_text(cmd.exchange_client_order_id);
    if (!by_venue_id && coid.empty()) {
        return std::unexpected{BuildError::NoOrderTarget};
    }
    const auto ts = static_cast<std::int64_t>(now.ns / 1'000'000ULL) - k_clock_backdate_ms;

    Query query{query_};
    if (by_venue_id) {
        query.put("orderId", venue_id);
    } else {
        query.put("clientOrderID", coid).put("clientOrderId", coid);
    }
    query.put("quantity", *qty)
        .put("recvWindow", k_recv_window)
        .put("symbol", base.exchange_symbol.view())
        .put("timestamp", ts);
    if (!query.ok()) {
        return std::unexpected{BuildError::UnsafeParameter};
    }
    return sign(net::HttpMethod::Post, ep::k_perp_amend, query.view());
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::open_orders(MarketType market,
                                                                              UnixNanos now) {
    const auto ts = static_cast<std::int64_t>(now.ns / 1'000'000ULL) - k_clock_backdate_ms;
    Query query{query_};
    query.put("recvWindow", k_recv_window).put("timestamp", ts);
    if (!query.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    switch (market) {
    case MarketType::Perp:
        return sign(net::HttpMethod::Get, ep::k_perp_open_orders, query.view());
    case MarketType::Spot:
        return sign(net::HttpMethod::Get, ep::k_spot_open_orders, query.view());
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
        break;
    case MarketType::Spot:
        return std::unexpected{BuildError::SpotPositionsUnsupported};
    case MarketType::Future:
        return std::unexpected{BuildError::DatedFuturesUnsupported};
    case MarketType::Option:
        return std::unexpected{BuildError::OptionsUnsupported};
    }
    const auto ts = static_cast<std::int64_t>(now.ns / 1'000'000ULL) - k_clock_backdate_ms;
    Query query{query_};
    query.put("recvWindow", k_recv_window).put("timestamp", ts);
    if (!query.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Get, ep::k_perp_positions, query.view());
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::position_mode(UnixNanos now) {
    const auto ts = static_cast<std::int64_t>(now.ns / 1'000'000ULL) - k_clock_backdate_ms;
    Query query{query_};
    query.put("recvWindow", k_recv_window).put("timestamp", ts);
    if (!query.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Get, ep::k_position_mode, query.view());
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::balance(UnixNanos now) {
    const auto ts = static_cast<std::int64_t>(now.ns / 1'000'000ULL) - k_clock_backdate_ms;
    Query query{query_};
    query.put("recvWindow", k_recv_window).put("timestamp", ts);
    if (!query.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Get, ep::k_perp_balance, query.view());
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::funding_income(UnixNanos since,
                                                                                 UnixNanos now) {
    const auto ts = static_cast<std::int64_t>(now.ns / 1'000'000ULL) - k_clock_backdate_ms;
    Query query{query_};
    // ascii order is bingx's convention; digest and wire share one buffer
    query.put("limit", k_income_limit).put("recvWindow", k_recv_window);
    if (since.ns != 0) {
        query.put("startTime", static_cast<std::int64_t>(since.ns / 1'000'000ULL));
    }
    query.put("timestamp", ts);
    if (!query.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Get, ep::k_perp_income, query.view());
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::listen_key_create(UnixNanos now) {
    const auto ts = static_cast<std::int64_t>(now.ns / 1'000'000ULL) - k_clock_backdate_ms;
    Query query{query_};
    query.put("recvWindow", k_recv_window).put("timestamp", ts);
    if (!query.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Post, ep::k_listen_key, query.view());
}

std::expected<net::RestRequest, BuildError>
SignedRequestBuilder::listen_key_keepalive(std::string_view key, UnixNanos now) {
    const auto ts = static_cast<std::int64_t>(now.ns / 1'000'000ULL) - k_clock_backdate_ms;
    Query query{query_};
    query.put("listenKey", key).put("recvWindow", k_recv_window).put("timestamp", ts);
    if (!query.ok()) {
        return std::unexpected{BuildError::UnsafeParameter};
    }
    return sign(net::HttpMethod::Put, ep::k_listen_key, query.view());
}

std::expected<net::RestRequest, BuildError>
SignedRequestBuilder::listen_key_close(std::string_view key, UnixNanos now) {
    const auto ts = static_cast<std::int64_t>(now.ns / 1'000'000ULL) - k_clock_backdate_ms;
    Query query{query_};
    query.put("listenKey", key).put("recvWindow", k_recv_window).put("timestamp", ts);
    if (!query.ok()) {
        return std::unexpected{BuildError::UnsafeParameter};
    }
    return sign(net::HttpMethod::Delete, ep::k_listen_key, query.view());
}

} // namespace raikiri::venues::bingx
