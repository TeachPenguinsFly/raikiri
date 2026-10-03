#include <raikiri/net/json_writer.hpp>
#include <raikiri/venues/weex/endpoints.hpp>
#include <raikiri/venues/weex/rest.hpp>

#include <array>
#include <charconv>
#include <cstring>
#include <span>

namespace raikiri::venues::weex {

namespace {

// the wire uses hyphens; the error table spells them with underscores
constexpr std::string_view k_hdr_key = "ACCESS-KEY";
constexpr std::string_view k_hdr_sign = "ACCESS-SIGN";
constexpr std::string_view k_hdr_timestamp = "ACCESS-TIMESTAMP";
constexpr std::string_view k_hdr_passphrase = "ACCESS-PASSPHRASE";

// the venue's page ceiling on both list reads
constexpr std::string_view k_page_limit = "100";

// ask for funding rows by type; a page of other bills may hold none
constexpr std::string_view k_income_funding = "position_funding";

// weex refuses fill windows over 7 days and bill windows over 100, so clamp
constexpr std::uint64_t k_max_fill_window_ms = 7ULL * 24 * 60 * 60 * 1000;
constexpr std::uint64_t k_max_bill_window_ms = 100ULL * 24 * 60 * 60 * 1000;

// reports overflow instead of truncating a signed payload
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
    return side == Side::Buy ? "BUY" : "SELL";
}

// weex requires positionSide LONG or SHORT: an opening order faces its side, a reduce-only one the opposite
// unverified under separated mode, which this adapter does not read
[[nodiscard]] std::string_view position_side_text(Side side, OrderFlags flags) noexcept {
    const bool closing = has_flag(flags, OrderFlags::ReduceOnly);
    const bool facing_long = (side == Side::Buy) != closing;
    return facing_long ? "LONG" : "SHORT";
}

// post-only is a time in force value on weex
[[nodiscard]] std::string_view tif_text(TimeInForce tif, OrderFlags flags) noexcept {
    if (has_flag(flags, OrderFlags::PostOnly)) {
        return "POST_ONLY";
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

// contract tree only; weex would answer a spot symbol with -1121
[[nodiscard]] bool serves(MarketType market) noexcept {
    return market == MarketType::Perp;
}

} // namespace

std::string_view rest_host() noexcept {
    return ep::k_rest_host;
}

std::string_view to_string(BuildError err) noexcept {
    switch (err) {
    case BuildError::UnknownInstrument:
        return "unknown instrument";
    case BuildError::UnsupportedMarket:
        return "weex serves the contract tree only";
    case BuildError::BadValue:
        return "bad price or quantity";
    case BuildError::NoOrderTarget:
        return "cancel names no order";
    case BuildError::BufferTooSmall:
        return "buffer too small";
    case BuildError::NotSigned:
        return "credentials not loaded";
    }
    return "unknown";
}

net::RestRequest RequestBuilder::instruments() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_exchange_info};
}

net::RestRequest RequestBuilder::api_trading_symbols() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_api_trading_symbols};
}

net::RestRequest RequestBuilder::premium_index() noexcept {
    // no symbol: one body covers every contract
    return {.method = net::HttpMethod::Get, .target = ep::k_premium_index};
}

net::RestRequest RequestBuilder::server_time() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_server_time};
}

std::string_view RequestBuilder::last_sign_message() const noexcept {
    return {sign_msg_.data(), sign_msg_len_};
}

std::expected<net::RestRequest, BuildError> RequestBuilder::sign(
    net::HttpMethod method, std::string_view path, std::string_view query, std::string_view body, UnixNanos now) {
    if (signer_ == nullptr || !signer_->has_credentials() || passphrase_.empty()) {
        return std::unexpected{BuildError::NotSigned};
    }
    Appender ts{timestamp_};
    ts << now.ns / 1'000'000ULL;
    if (!ts.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }

    // the signer adds "?" only when a query exists
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
    request.headers = {{k_hdr_key, signer_->api_key()},
                       {k_hdr_sign, signature},
                       {k_hdr_timestamp, ts.view()},
                       {k_hdr_passphrase, passphrase_}};
    // weex refuses any other content type with -1045, even on a GET
    request.content_type = "application/json";
    return request;
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::create_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
    if (!serves(cmd.instrument.market())) {
        return std::unexpected{BuildError::UnsupportedMarket};
    }
    const auto* instrument = table.find(cmd.instrument);
    if (instrument == nullptr) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    const auto& base = base_of(*instrument);
    const bool is_limit = cmd.order_type == OrderType::Limit;
    // undef must not render as "0"
    if (cmd.qty.is_undef() || (is_limit && cmd.price.is_undef())) {
        return std::unexpected{BuildError::BadValue};
    }
    if (cmd.exchange_client_order_id.empty()) {
        // newClientOrderId is required
        return std::unexpected{BuildError::NoOrderTarget};
    }

    net::JsonWriter json{body_};
    json.obj_begin()
        .str("symbol", table.exchange_symbol(cmd.instrument))
        .str("side", side_text(cmd.side))
        .str("positionSide", position_side_text(cmd.side, cmd.flags))
        .str("type", is_limit ? "LIMIT" : "MARKET")
        .decimal("quantity", base.lots_to_qty(cmd.qty));
    if (is_limit) {
        json.str("timeInForce", tif_text(cmd.tif, cmd.flags)).decimal("price", base.ticks_to_price(cmd.price));
    }
    json.str("newClientOrderId", cmd.exchange_client_order_id.view());
    if (has_flag(cmd.flags, OrderFlags::ReduceOnly)) {
        json.boolean("reduceOnly", true);
    }
    json.obj_end();

    if (!json.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Post, ep::k_order, {}, json.view(), now);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::cancel_order(const OrderCmd& cmd, UnixNanos now) {
    // weex cancels by id alone
    Appender query{query_};
    if (!cmd.target_exchange_order_id.empty()) {
        query << "orderId=" << cmd.target_exchange_order_id.view();
    } else if (!cmd.exchange_client_order_id.empty()) {
        query << "origClientOrderId=" << cmd.exchange_client_order_id.view();
    } else {
        return std::unexpected{BuildError::NoOrderTarget};
    }
    if (!query.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Delete, ep::k_order, query.view(), {}, now);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::cancel_batch(std::span<const OrderCmd> cmds,
                                                                         UnixNanos now) {
    if (cmds.empty() || cmds.size() > k_max_cancel_batch) {
        return std::unexpected{BuildError::NoOrderTarget};
    }
    // by client id: every order placed carries a ulid
    Appender list{list_};
    list << "[";
    for (std::size_t i = 0; i < cmds.size(); ++i) {
        if (cmds[i].exchange_client_order_id.empty()) {
            return std::unexpected{BuildError::NoOrderTarget};
        }
        if (i != 0) {
            list << ",";
        }
        list << "\"" << cmds[i].exchange_client_order_id.view() << "\"";
    }
    list << "]";
    if (!list.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }

    net::JsonWriter json{body_};
    json.obj_begin().raw("origClientOrderIdList", list.view()).obj_end();
    if (!json.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Delete, ep::k_batch_orders, {}, json.view(), now);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::open_orders(MarketType market, UnixNanos now) {
    if (!serves(market)) {
        return std::unexpected{BuildError::UnsupportedMarket};
    }
    Appender query{query_};
    query << "limit=" << k_page_limit << "&page=0";
    if (!query.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Get, ep::k_open_orders, query.view(), {}, now);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::fills(MarketType market, UnixNanos since, UnixNanos now) {
    if (!serves(market)) {
        return std::unexpected{BuildError::UnsupportedMarket};
    }
    Appender query{query_};
    query << "limit=" << k_page_limit;
    if (since.ns != 0) {
        const std::uint64_t end_ms = now.ns / 1'000'000ULL;
        std::uint64_t start_ms = since.ns / 1'000'000ULL;
        if (end_ms < start_ms || end_ms - start_ms > k_max_fill_window_ms) {
            start_ms = end_ms > k_max_fill_window_ms ? end_ms - k_max_fill_window_ms : 0;
        }
        query << "&startTime=" << start_ms << "&endTime=" << end_ms;
    }
    // no since means weex's default of the last seven days
    if (!query.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Get, ep::k_user_trades, query.view(), {}, now);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::positions(MarketType market, UnixNanos now) {
    if (!serves(market)) {
        return std::unexpected{BuildError::UnsupportedMarket};
    }
    return sign(net::HttpMethod::Get, ep::k_all_positions, {}, {}, now);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::balances(UnixNanos now) {
    // one collateral pool backs every contract
    return sign(net::HttpMethod::Get, ep::k_balance, {}, {}, now);
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::funding_charges(MarketType market, UnixNanos since, UnixNanos now) {
    if (!serves(market)) {
        return std::unexpected{BuildError::UnsupportedMarket};
    }
    const std::uint64_t end_ms = now.ns / 1'000'000ULL;
    std::uint64_t start_ms = since.ns / 1'000'000ULL;
    if (start_ms == 0 || end_ms < start_ms || end_ms - start_ms > k_max_bill_window_ms) {
        start_ms = end_ms > k_max_bill_window_ms ? end_ms - k_max_bill_window_ms : 0;
    }

    net::JsonWriter json{body_};
    json.obj_begin()
        .str("incomeType", k_income_funding)
        .num("startTime", static_cast<std::int64_t>(start_ms))
        .num("endTime", static_cast<std::int64_t>(end_ms))
        .num("limit", 100)
        .obj_end();
    if (!json.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Post, ep::k_income, {}, json.view(), now);
}

} // namespace raikiri::venues::weex
