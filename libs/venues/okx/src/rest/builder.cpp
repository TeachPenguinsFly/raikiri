#include <raikiri/net/json_writer.hpp>
#include <raikiri/venues/okx/endpoints.hpp>
#include <raikiri/venues/okx/rest.hpp>

#include <algorithm>
#include <charconv>
#include <cstring>

namespace raikiri::venues::okx {

namespace {

// Header names are OKX's, capitalisation and all.
constexpr std::string_view k_hdr_key = "OK-ACCESS-KEY";
constexpr std::string_view k_hdr_sign = "OK-ACCESS-SIGN";
constexpr std::string_view k_hdr_ts = "OK-ACCESS-TIMESTAMP";
constexpr std::string_view k_hdr_passphrase = "OK-ACCESS-PASSPHRASE";
constexpr std::string_view k_hdr_simulated = "x-simulated-trading";

// Fills the builder's target buffer. A part that would overflow is dropped
// rather than truncated mid-token, so an over-long instId yields a request
// OKX refuses with 51001 instead of one that names a different instrument.
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

    void put(std::uint64_t value) noexcept {
        std::array<char, 24> num{};
        if (const auto [stop, ec] = std::to_chars(num.data(), num.data() + num.size(), value);
            ec == std::errc{}) {
            put(std::string_view{num.data(), static_cast<std::size_t>(stop - num.data())});
        }
    }

    [[nodiscard]] std::string_view view() const noexcept {
        return std::string_view{into_.data(), len_};
    }

    [[nodiscard]] net::RestRequest get() const noexcept {
        return {.method = net::HttpMethod::Get, .target = view(), .body = {}};
    }

private:
    std::span<char> into_;
    std::size_t len_{0};
};

// Appends into a fixed buffer, reporting overflow rather than truncating: a
// truncated signed message authenticates something we did not mean to send.
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

// OKX has no timeInForce field: post-only, IOC and FOK ARE order types.
// Sending "limit" with a post-only flag beside it is accepted and places a
// crossing order, which is the expensive way to learn this.
[[nodiscard]] std::string_view
ord_type_text(OrderType type, TimeInForce tif, OrderFlags flags) noexcept {
    switch (type) {
    case OrderType::Market:
        // Market is already immediate-or-cancel, and OKX has no post-only
        // market order to express.
        return "market";
    case OrderType::Limit:
        break;
    }
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
    return "limit";
}

[[nodiscard]] std::string_view side_text(Side side) noexcept {
    return side == Side::Buy ? "buy" : "sell";
}

void put_digits(std::span<char> out, std::size_t at, std::uint64_t value, int width) noexcept {
    for (int i = width - 1; i >= 0; --i) {
        out[at + static_cast<std::size_t>(i)] = static_cast<char>('0' + value % 10);
        value /= 10;
    }
}

} // namespace

std::string_view to_string(BuildError err) noexcept {
    switch (err) {
    case BuildError::UnknownInstrument:
        return "unknown instrument";
    case BuildError::UnsupportedMarket:
        return "market not supported for order entry";
    case BuildError::BadQuantity:
        return "bad quantity";
    case BuildError::BadPrice:
        return "bad price";
    case BuildError::BufferTooSmall:
        return "buffer too small";
    case BuildError::NotSigned:
        return "credentials not loaded";
    case BuildError::NothingToSend:
        return "nothing to send";
    }
    return "unknown";
}

std::string_view inst_type_of(MarketType market) noexcept {
    switch (market) {
    case MarketType::Spot:
        return "SPOT";
    case MarketType::Perp:
        return "SWAP";
    case MarketType::Future:
        return "FUTURES";
    case MarketType::Option:
        return "OPTION";
    }
    return {};
}

std::expected<std::string_view, BuildError> td_mode_of(MarketType market) noexcept {
    switch (market) {
    case MarketType::Spot:
        return "cash";
    case MarketType::Perp:
    case MarketType::Future:
        return "cross";
    case MarketType::Option:
        return std::unexpected{BuildError::UnsupportedMarket};
    }
    return std::unexpected{BuildError::UnsupportedMarket};
}

std::string_view rest_host() noexcept {
    return ep::rest_host();
}

std::string_view iso8601_ms(UnixNanos now, std::span<char, 24> out) noexcept {
    const auto ms = now.ns / 1'000'000ULL;
    const auto days = static_cast<std::int64_t>(ms / 86'400'000ULL);
    const auto in_day = ms % 86'400'000ULL;

    // days-from-civil, inverted (Howard Hinnant). Integer throughout: a
    // timestamp computed in floating point loses milliseconds around 2038 and
    // signs a string the venue then disagrees with.
    auto z = days + 719'468;
    const auto era = (z >= 0 ? z : z - 146'096) / 146'097;
    const auto doe = z - era * 146'097;
    const auto yoe = (doe - doe / 1460 + doe / 36'524 - doe / 146'096) / 365;
    const auto doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const auto mp = (5 * doy + 2) / 153;
    const auto day = doy - (153 * mp + 2) / 5 + 1;
    const auto month = mp + (mp < 10 ? 3 : -9);
    const auto year = yoe + era * 400 + (month <= 2 ? 1 : 0);

    put_digits(out, 0, static_cast<std::uint64_t>(year), 4);
    out[4] = '-';
    put_digits(out, 5, static_cast<std::uint64_t>(month), 2);
    out[7] = '-';
    put_digits(out, 8, static_cast<std::uint64_t>(day), 2);
    out[10] = 'T';
    put_digits(out, 11, in_day / 3'600'000ULL, 2);
    out[13] = ':';
    put_digits(out, 14, in_day % 3'600'000ULL / 60'000ULL, 2);
    out[16] = ':';
    put_digits(out, 17, in_day % 60'000ULL / 1'000ULL, 2);
    out[19] = '.';
    put_digits(out, 20, in_day % 1'000ULL, 3);
    out[23] = 'Z';
    return {out.data(), out.size()};
}

net::RestRequest RequestBuilder::server_time() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_server_time, .body = {}};
}

// Every builder below writes into the same buffer, so a request must be sent
// before the next one is built. One catalogue load at a time is the only use.
net::RestRequest RequestBuilder::instruments(std::string_view inst_type) noexcept {
    TargetWriter writer{target_};
    writer.put(ep::k_instruments);
    writer.put("?instType=");
    writer.put(inst_type);
    return writer.get();
}

net::RestRequest RequestBuilder::mark_price(std::string_view inst_id) noexcept {
    TargetWriter writer{target_};
    writer.put(ep::k_mark_price);
    writer.put("?instType=SWAP&instId=");
    writer.put(inst_id);
    return writer.get();
}

net::RestRequest RequestBuilder::funding_rate(std::string_view inst_id) noexcept {
    TargetWriter writer{target_};
    writer.put(ep::k_funding_rate);
    writer.put("?instId=");
    writer.put(inst_id);
    return writer.get();
}

net::RestRequest RequestBuilder::books(std::string_view inst_id, std::uint32_t depth) noexcept {
    TargetWriter writer{target_};
    writer.put(ep::k_books);
    writer.put("?instId=");
    writer.put(inst_id);
    writer.put("&sz=");
    writer.put(depth);
    return writer.get();
}

std::string_view RequestBuilder::last_sign_message() const noexcept {
    return {sign_msg_.data(), sign_msg_len_};
}

std::vector<net::HttpHeader> RequestBuilder::headers(std::string_view timestamp,
                                                     std::string_view signature) const {
    std::vector<net::HttpHeader> out{{k_hdr_key, signer_->api_key()},
                                     {k_hdr_sign, signature},
                                     {k_hdr_ts, timestamp},
                                     {k_hdr_passphrase, passphrase_}};
    if (simulated_) {
        out.push_back({k_hdr_simulated, "1"});
    }
    return out;
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::signed_post(std::string_view path, std::string_view body, UnixNanos now) {
    if (signer_ == nullptr || !signer_->has_credentials() || passphrase_.empty()) {
        return std::unexpected{BuildError::NotSigned};
    }
    const auto ts = iso8601_ms(now, timestamp_);

    Appender msg{sign_msg_};
    msg << ts << "POST" << path << body;
    if (!msg.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    sign_msg_len_ = msg.view().size();

    const auto signature = signer_->sign(msg.view(), net::Signer::Encoding::Base64, signature_);
    if (signature.empty()) {
        return std::unexpected{BuildError::NotSigned};
    }

    net::RestRequest request{};
    request.method = net::HttpMethod::Post;
    request.target = path;
    request.body = body;
    request.headers = headers(ts, signature);
    return request;
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::signed_get(std::string_view path, std::string_view query, UnixNanos now) {
    if (signer_ == nullptr || !signer_->has_credentials() || passphrase_.empty()) {
        return std::unexpected{BuildError::NotSigned};
    }
    TargetWriter target{target_};
    target.put(path);
    if (!query.empty()) {
        target.put("?");
        target.put(query);
    }
    if (target.view().size() != path.size() + (query.empty() ? 0 : query.size() + 1)) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    const auto ts = iso8601_ms(now, timestamp_);

    // The whole target, '?' and query included — OKX signs a GET's parameters
    // as part of requestPath. There is no body to append.
    Appender msg{sign_msg_};
    msg << ts << "GET" << target.view();
    if (!msg.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    sign_msg_len_ = msg.view().size();

    const auto signature = signer_->sign(msg.view(), net::Signer::Encoding::Base64, signature_);
    if (signature.empty()) {
        return std::unexpected{BuildError::NotSigned};
    }

    net::RestRequest request{};
    request.method = net::HttpMethod::Get;
    request.target = target.view();
    request.body = {};
    request.headers = headers(ts, signature);
    return request;
}

std::expected<net::RestRequest, BuildError> RequestBuilder::create_order(
    const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
    const auto market = cmd.instrument.market();
    const auto td_mode = td_mode_of(market);
    if (!td_mode.has_value()) {
        return std::unexpected{td_mode.error()};
    }
    const auto* instrument = table.find(cmd.instrument);
    if (instrument == nullptr) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    const auto& base = base_of(*instrument);
    if (cmd.qty.is_undef()) {
        return std::unexpected{BuildError::BadQuantity};
    }
    if (cmd.order_type == OrderType::Limit && cmd.price.is_undef()) {
        return std::unexpected{BuildError::BadPrice};
    }

    net::JsonWriter json{body_};
    json.obj_begin()
        .str("instId", base.exchange_symbol.view())
        .str("tdMode", *td_mode)
        .str("clOrdId", cmd.exchange_client_order_id.view())
        .str("side", side_text(cmd.side))
        .str("ordType", ord_type_text(cmd.order_type, cmd.tif, cmd.flags))
        .decimal("sz", base.lots_to_qty(cmd.qty));
    if (cmd.order_type == OrderType::Limit) {
        json.decimal("px", base.ticks_to_price(cmd.price));
    }
    switch (market) {
    case MarketType::Spot:
        // A SPOT market order defaults to sizing in the QUOTE currency on a
        // buy. Our lots are base units, so omitting tgtCcy turns "buy 0.003
        // BTC" into "spend 0.003 USDT" — an order 20000x too small that the
        // venue accepts without complaint.
        if (cmd.order_type == OrderType::Market) {
            json.str("tgtCcy", "base_ccy");
        }
        break;
    case MarketType::Perp:
    case MarketType::Future:
        // reduceOnly is rejected outright on SPOT, and posSide is deliberately
        // absent: the account is assumed to be in net mode, and long/short
        // mode refuses a derivatives order without it (51000).
        if (has_flag(cmd.flags, OrderFlags::ReduceOnly)) {
            json.boolean("reduceOnly", true);
        }
        break;
    case MarketType::Option:
        // Unreachable: td_mode_of refused above. Named anyway so the arm has
        // to be revisited the day options gain a tick size and a price type.
        return std::unexpected{BuildError::UnsupportedMarket};
    }
    json.obj_end();

    if (!json.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return signed_post(ep::k_order_create, json.view(), now);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::cancel_order(
    const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
    // A cancel names an existing order, so it needs no trade mode and options
    // would be cancellable if we could ever have opened one.
    const auto symbol = table.exchange_symbol(cmd.instrument);
    if (symbol.empty()) {
        return std::unexpected{BuildError::UnknownInstrument};
    }

    net::JsonWriter json{body_};
    json.obj_begin().str("instId", symbol);
    if (!cmd.target_exchange_order_id.empty()) {
        json.str("ordId", cmd.target_exchange_order_id.view());
    } else {
        json.str("clOrdId", cmd.exchange_client_order_id.view());
    }
    json.obj_end();

    if (!json.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return signed_post(ep::k_order_cancel, json.view(), now);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::amend_order(
    const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
    const auto* instrument = table.find(cmd.instrument);
    if (instrument == nullptr) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    const auto& base = base_of(*instrument);
    if (cmd.qty.is_undef() && cmd.price.is_undef()) {
        return std::unexpected{BuildError::BadQuantity}; // OKX needs one of the two
    }

    net::JsonWriter json{body_};
    json.obj_begin().str("instId", base.exchange_symbol.view());
    if (!cmd.target_exchange_order_id.empty()) {
        json.str("ordId", cmd.target_exchange_order_id.view());
    } else {
        json.str("clOrdId", cmd.exchange_client_order_id.view());
    }
    if (!cmd.qty.is_undef()) {
        // newSz is the order's new TOTAL size, not its new remainder: on a
        // partially filled order, passing the remainder shrinks it by
        // everything already done.
        json.decimal("newSz", base.lots_to_qty(cmd.qty));
    }
    if (!cmd.price.is_undef()) {
        json.decimal("newPx", base.ticks_to_price(cmd.price));
    }
    json.obj_end();

    if (!json.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return signed_post(ep::k_order_amend, json.view(), now);
}

std::expected<RequestBuilder::Batch, BuildError> RequestBuilder::cancel_batch(
    std::span<const OrderCmd> cmds, const exec::InstrumentTable& table, UnixNanos now) {
    if (cmds.empty()) {
        return std::unexpected{BuildError::NothingToSend};
    }
    // OKX caps a cancel batch at 20 and rejects an oversized frame whole.
    constexpr std::size_t k_max_per_batch = 20;
    const auto batch = std::min(cmds.size(), k_max_per_batch);

    net::JsonWriter json{body_};
    json.arr_begin();
    std::size_t consumed = 0;
    for (std::size_t i = 0; i < batch; ++i) {
        const auto& cmd = cmds[i];
        const auto symbol = table.exchange_symbol(cmd.instrument);
        if (symbol.empty()) {
            // Dropping it silently would leave the order live with nobody
            // waiting for a verdict; the caller reports the tail as rejected.
            break;
        }
        json.obj_begin().str("instId", symbol);
        if (!cmd.target_exchange_order_id.empty()) {
            json.str("ordId", cmd.target_exchange_order_id.view());
        } else {
            json.str("clOrdId", cmd.exchange_client_order_id.view());
        }
        json.obj_end();
        ++consumed;
    }
    json.arr_end();

    if (consumed == 0) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    if (!json.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    const auto request = signed_post(ep::k_order_cancel_batch, json.view(), now);
    if (!request.has_value()) {
        return std::unexpected{request.error()};
    }
    return Batch{.request = *request, .consumed = consumed};
}

std::expected<net::RestRequest, BuildError> RequestBuilder::open_orders(MarketType market,
                                                                        UnixNanos now) {
    Appender query{body_}; // reuse the body buffer: a GET sends none
    query << "instType=" << inst_type_of(market);
    if (!query.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return signed_get(ep::k_orders_pending, query.view(), now);
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::fills(MarketType market, UnixNanos since, UnixNanos now) {
    std::array<char, 24> since_text{};
    std::string_view begin;
    if (since.ns != 0) {
        const auto ms = since.ns / 1'000'000ULL;
        if (const auto [stop, ec] =
                std::to_chars(since_text.data(), since_text.data() + since_text.size(), ms);
            ec == std::errc{}) {
            begin = {since_text.data(), static_cast<std::size_t>(stop - since_text.data())};
        }
    }
    Appender query{body_};
    query << "instType=" << inst_type_of(market);
    if (!begin.empty()) {
        query << "&begin=" << begin;
    }
    if (!query.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return signed_get(ep::k_fills, query.view(), now);
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::funding_book(MarketType market, UnixNanos since, UnixNanos now) {
    std::array<char, 24> since_text{};
    std::string_view begin;
    if (since.ns != 0) {
        const auto ms = since.ns / 1'000'000ULL;
        if (const auto [stop, ec] =
                std::to_chars(since_text.data(), since_text.data() + since_text.size(), ms);
            ec == std::errc{}) {
            begin = {since_text.data(), static_cast<std::size_t>(stop - since_text.data())};
        }
    }
    Appender query{body_};
    query << "instType=" << inst_type_of(market);
    if (!begin.empty()) {
        query << "&begin=" << begin;
    }
    if (!query.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return signed_get(ep::k_bills, query.view(), now);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::balances(UnixNanos now) {
    return signed_get(ep::k_balance, {}, now);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::account_config(UnixNanos now) {
    return signed_get(ep::k_account_config, {}, now);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::positions(MarketType market,
                                                                      UnixNanos now) {
    Appender query{body_};
    query << "instType=" << inst_type_of(market);
    if (!query.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return signed_get(ep::k_positions, query.view(), now);
}

} // namespace raikiri::venues::okx
