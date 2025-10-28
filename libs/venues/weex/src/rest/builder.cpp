#include <raikiri/net/json_writer.hpp>
#include <raikiri/venues/weex/endpoints.hpp>
#include <raikiri/venues/weex/rest.hpp>

#include <array>
#include <charconv>
#include <cstring>
#include <span>

namespace raikiri::venues::weex {

namespace {

// Header names are WEEX's, hyphens and capitals and all. The error table spells
// them with underscores (ACCESS_KEY); the wire wants the hyphen.
constexpr std::string_view k_hdr_key = "ACCESS-KEY";
constexpr std::string_view k_hdr_sign = "ACCESS-SIGN";
constexpr std::string_view k_hdr_timestamp = "ACCESS-TIMESTAMP";
constexpr std::string_view k_hdr_passphrase = "ACCESS-PASSPHRASE";

// The venue's own page ceiling on both list reads.
constexpr std::string_view k_page_limit = "100";

// Funding rows are asked for by type rather than filtered afterwards: the bill
// list answers every deposit, transfer and fee the account ever made, and a
// hundred-row page of those could contain no settlement at all.
constexpr std::string_view k_income_funding = "position_funding";

// WEEX refuses a fills window wider than seven days and a bill window wider
// than a hundred, and answers an over-wide request with an error rather than a
// truncated page. Clamped here, because a silently short answer reads as "no
// fills" and "no funding was charged".
constexpr std::uint64_t k_max_fill_window_ms = 7ULL * 24 * 60 * 60 * 1000;
constexpr std::uint64_t k_max_bill_window_ms = 100ULL * 24 * 60 * 60 * 1000;

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
    return side == Side::Buy ? "BUY" : "SELL";
}

// WEEX REQUIRES A POSITION SIDE ON EVERY ORDER and its vocabulary is LONG and
// SHORT with no "both" — the value a one-way account would otherwise carry. So
// it is DERIVED, and this is the single most likely thing in this file to be
// wrong the first time a credential reaches the venue:
//
//   an opening order faces the way it trades — a buy opens a long;
//   a reduce-only order faces the position it is closing, which is the OPPOSITE
//   of the way it trades — a buy closes a short.
//
// Under `separatedMode` COMBINED, which is the one-way account, both readings
// name the one position the account has and the field is a formality. Under
// SEPARATED they name different legs, and a wrong one opens a hedge instead of
// closing a position. Nothing here can tell which mode an account is in —
// /capi/v3/account/accountConfig says, and this adapter does not read it.
[[nodiscard]] std::string_view position_side_text(Side side, OrderFlags flags) noexcept {
    const bool closing = has_flag(flags, OrderFlags::ReduceOnly);
    const bool facing_long = (side == Side::Buy) != closing;
    return facing_long ? "LONG" : "SHORT";
}

// Post-only is a TIME IN FORCE value on WEEX, not a separate boolean. Sending
// it as a flag is ignored and the order crosses.
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

// The contract tree and nothing else. Answered before a request exists rather
// than left to the venue, which would take a spot symbol it does not list and
// answer -1121.
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
    // NO `symbol`: the bare path answers for every contract in one body, which
    // is one call a minute rather than one per instrument watched.
    return {.method = net::HttpMethod::Get, .target = ep::k_premium_index};
}

net::RestRequest RequestBuilder::server_time() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_server_time};
}

std::string_view RequestBuilder::last_sign_message() const noexcept {
    return {sign_msg_.data(), sign_msg_len_};
}

std::expected<net::RestRequest, BuildError> RequestBuilder::sign(net::HttpMethod method,
                                                                 std::string_view path,
                                                                 std::string_view query,
                                                                 std::string_view body,
                                                                 UnixNanos now) {
    if (signer_ == nullptr || !signer_->has_credentials() || passphrase_.empty()) {
        return std::unexpected{BuildError::NotSigned};
    }
    Appender ts{timestamp_};
    ts << now.ns / 1'000'000ULL;
    if (!ts.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }

    // The "?" belongs to the SIGNER and not to the query: a query carrying its
    // own leading "?" doubles it here, and an empty query that got one anyway
    // signs a string the venue never sees.
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
    // WEEX refuses anything else with -1045, on a GET with no body as much as
    // on a POST with one.
    request.content_type = "application/json";
    return request;
}

std::expected<net::RestRequest, BuildError> RequestBuilder::create_order(
    const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
    if (!serves(cmd.instrument.market())) {
        return std::unexpected{BuildError::UnsupportedMarket};
    }
    const auto* instrument = table.find(cmd.instrument);
    if (instrument == nullptr) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    const auto& base = base_of(*instrument);
    const bool is_limit = cmd.order_type == OrderType::Limit;
    // UNDEF IS NOT ZERO, and a quantity the OMS never filled in must not be
    // rendered as one: WEEX would refuse "0" with -1142 on a good day and take
    // it on a bad one.
    if (cmd.qty.is_undef() || (is_limit && cmd.price.is_undef())) {
        return std::unexpected{BuildError::BadValue};
    }
    if (cmd.exchange_client_order_id.empty()) {
        // `newClientOrderId` is REQUIRED on this venue, so an order with no id
        // cannot be sent at all — and one sent without would come back
        // unattributable.
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
        json.str("timeInForce", tif_text(cmd.tif, cmd.flags))
            .decimal("price", base.ticks_to_price(cmd.price));
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

std::expected<net::RestRequest, BuildError> RequestBuilder::cancel_order(const OrderCmd& cmd,
                                                                         UnixNanos now) {
    // NO SYMBOL AND NO MARKET: WEEX cancels by id alone, which is why this one
    // builder needs neither the instrument table nor a live catalogue.
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

std::expected<net::RestRequest, BuildError>
RequestBuilder::cancel_batch(std::span<const OrderCmd> cmds, UnixNanos now) {
    if (cmds.empty() || cmds.size() > k_max_cancel_batch) {
        return std::unexpected{BuildError::NoOrderTarget};
    }
    // BY CLIENT ID ONLY. WEEX takes `orderIdList` or `origClientOrderIdList`
    // and this sends the second: every order this process placed carries a
    // ULID — the venue requires one — while the venue's own id is known only
    // for orders it has already answered about.
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

std::expected<net::RestRequest, BuildError> RequestBuilder::open_orders(MarketType market,
                                                                        UnixNanos now) {
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

std::expected<net::RestRequest, BuildError>
RequestBuilder::fills(MarketType market, UnixNanos since, UnixNanos now) {
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
    // A caller that named no `since` sends no window at all, which is WEEX's
    // own default of the last seven days rather than a window invented here.
    if (!query.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Get, ep::k_user_trades, query.view(), {}, now);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::positions(MarketType market,
                                                                      UnixNanos now) {
    if (!serves(market)) {
        return std::unexpected{BuildError::UnsupportedMarket};
    }
    return sign(net::HttpMethod::Get, ep::k_all_positions, {}, {}, now);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::balances(UnixNanos now) {
    // NO MARKET. One collateral pool backs every contract on this host, so the
    // answer does not vary by market and narrowing it would be an invention.
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
