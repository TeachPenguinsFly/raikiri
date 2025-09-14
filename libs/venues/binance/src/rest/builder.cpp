#include <raikiri/venues/binance/endpoints.hpp>
#include <raikiri/venues/binance/rest.hpp>

#include <charconv>
#include <cstring>

namespace raikiri::venues::binance {

namespace {

constexpr std::string_view k_hdr_key = "X-MBX-APIKEY";
constexpr std::string_view k_form = "application/x-www-form-urlencoded";

// Appends into a fixed buffer and remembers whether it ever ran out. A
// truncated query would still be signed, and the signature would then
// authenticate an order that is not the one intended.
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

    [[nodiscard]] std::size_t size() const noexcept { return len_; }

    [[nodiscard]] std::string_view view() const noexcept { return {buf_.data(), len_}; }

private:
    std::span<char> buf_;
    std::size_t len_{0};
    bool overflow_{false};
};

[[nodiscard]] std::string_view side_text(Side side) noexcept {
    return side == Side::Buy ? "BUY" : "SELL";
}

// Post-only is a time-in-force value on binance (GTX), not a separate flag,
// so it displaces whatever the order asked for rather than accompanying it.
[[nodiscard]] std::string_view tif_text(TimeInForce tif, OrderFlags flags) noexcept {
    if (has_flag(flags, OrderFlags::PostOnly)) {
        return "GTX";
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

[[nodiscard]] std::uint64_t to_ms(UnixNanos ns) noexcept {
    return ns.ns / 1'000'000ULL;
}

// Renders an instrument-scaled integer back to the decimal text binance
// expects. Goes through FixedE9 rather than a float: a double round trip is
// exactly how a price arrives one tick away from the intended one.
[[nodiscard]] bool put_scaled(Appender& out, std::int64_t steps, FixedE9 step) noexcept {
    if (step.raw <= 0) {
        return false;
    }
    const auto value = FixedE9::from_raw(steps * step.raw);
    std::array<char, 40> buf{};
    const auto text = format_e9(value, buf.data(), buf.size());
    if (text.empty()) {
        return false;
    }
    out << text;
    return true;
}

} // namespace

std::string_view to_string(BuildError err) noexcept {
    switch (err) {
    case BuildError::NoCredentials:
        return "NoCredentials";
    case BuildError::BufferTooSmall:
        return "BufferTooSmall";
    case BuildError::UnsupportedOrderType:
        return "UnsupportedOrderType";
    case BuildError::BadValue:
        return "BadValue";
    }
    return "Unknown";
}

std::string_view rest_host(bool testnet) noexcept {
    return ep::rest_host(testnet);
}

net::RestRequest RequestBuilder::instruments() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_exchange_info, .body = {}};
}

// Every symbol in one call. Binance answers ~880 rows, which is cheaper than
// asking per instrument and is why this needs no round-robin cursor.
net::RestRequest RequestBuilder::ticker_24h() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_ticker_24h, .body = {}};
}

net::RestRequest RequestBuilder::premium_index() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_premium_index, .body = {}};
}

net::RestRequest RequestBuilder::server_time() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_server_time, .body = {}};
}

net::RestRequest RequestBuilder::depth_snapshot(std::string_view symbol,
                                                std::uint32_t limit) noexcept {
    Appender target{target_};
    target << ep::k_depth << "?symbol=" << symbol << "&limit=" << std::uint64_t{limit};
    return {.method = net::HttpMethod::Get,
            .target = target.ok() ? target.view() : std::string_view{},
            .body = {}};
}

std::string_view RequestBuilder::last_sign_message() const noexcept {
    return {query_.data(), sign_len_};
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::finish(net::HttpMethod method, std::string_view path, std::size_t query_len) {
    if (query_len == 0 || query_len > query_.size()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    sign_len_ = query_len;
    const std::string_view query{query_.data(), query_len};
    const auto signature = signer_->sign(query, net::Signer::Encoding::Hex, signature_);
    if (signature.empty()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }

    Appender target{target_};
    target << path << "?" << query << "&signature=" << signature;
    if (!target.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    net::RestRequest request{};
    request.method = method;
    request.target = target.view();
    request.body = {};
    request.content_type = k_form;
    request.headers = {{k_hdr_key, signer_->api_key()}};
    return request;
}

net::RestRequest RequestBuilder::header_only(net::HttpMethod method,
                                             std::string_view path) noexcept {
    net::RestRequest request{};
    request.method = method;
    request.target = path;
    request.body = {};
    request.content_type = k_form;
    request.headers = {{k_hdr_key, signer_->api_key()}};
    return request;
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::create_order(const OrderCmd& cmd, const InstrumentBase& instrument, UnixNanos now) {
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NoCredentials};
    }
    if (cmd.order_type == OrderType::Market && has_flag(cmd.flags, OrderFlags::PostOnly)) {
        return std::unexpected{BuildError::UnsupportedOrderType};
    }

    Appender q{query_};
    q << "symbol=" << instrument.exchange_symbol.view();
    q << "&side=" << side_text(cmd.side);
    q << "&type=" << (cmd.order_type == OrderType::Market ? "MARKET" : "LIMIT");
    if (cmd.order_type == OrderType::Limit) {
        q << "&timeInForce=" << tif_text(cmd.tif, cmd.flags);
    }
    q << "&quantity=";
    if (!put_scaled(q, cmd.qty.raw, instrument.lot_size)) {
        return std::unexpected{BuildError::BadValue};
    }
    if (cmd.order_type == OrderType::Limit) {
        q << "&price=";
        if (!put_scaled(q, cmd.price.raw, instrument.tick_size)) {
            return std::unexpected{BuildError::BadValue};
        }
    }
    q << "&newClientOrderId=" << cmd.exchange_client_order_id.view();
    if (has_flag(cmd.flags, OrderFlags::ReduceOnly)) {
        q << "&reduceOnly=true";
    }
    q << "&recvWindow=" << std::uint64_t{k_recv_window_ms};
    q << "&timestamp=" << to_ms(now);
    if (!q.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return finish(net::HttpMethod::Post, ep::k_order, q.size());
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::cancel_order(const OrderCmd& cmd, const InstrumentBase& instrument, UnixNanos now) {
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NoCredentials};
    }
    Appender q{query_};
    q << "symbol=" << instrument.exchange_symbol.view();
    if (!cmd.target_exchange_order_id.empty()) {
        q << "&orderId=" << cmd.target_exchange_order_id.view();
    } else {
        q << "&origClientOrderId=" << cmd.exchange_client_order_id.view();
    }
    q << "&recvWindow=" << std::uint64_t{k_recv_window_ms};
    q << "&timestamp=" << to_ms(now);
    if (!q.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return finish(net::HttpMethod::Delete, ep::k_order, q.size());
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::amend_order(const OrderCmd& cmd, const InstrumentBase& instrument, UnixNanos now) {
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NoCredentials};
    }
    Appender q{query_};
    q << "symbol=" << instrument.exchange_symbol.view();
    q << "&side=" << side_text(cmd.side);
    if (!cmd.target_exchange_order_id.empty()) {
        q << "&orderId=" << cmd.target_exchange_order_id.view();
    } else {
        q << "&origClientOrderId=" << cmd.exchange_client_order_id.view();
    }
    q << "&quantity=";
    if (!put_scaled(q, cmd.qty.raw, instrument.lot_size)) {
        return std::unexpected{BuildError::BadValue};
    }
    q << "&price=";
    if (!put_scaled(q, cmd.price.raw, instrument.tick_size)) {
        return std::unexpected{BuildError::BadValue};
    }
    q << "&recvWindow=" << std::uint64_t{k_recv_window_ms};
    q << "&timestamp=" << to_ms(now);
    if (!q.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return finish(net::HttpMethod::Put, ep::k_order, q.size());
}

std::expected<net::RestRequest, BuildError> RequestBuilder::open_orders(UnixNanos now) {
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NoCredentials};
    }
    Appender q{query_};
    q << "recvWindow=" << std::uint64_t{k_recv_window_ms} << "&timestamp=" << to_ms(now);
    if (!q.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return finish(net::HttpMethod::Get, ep::k_open_orders, q.size());
}

std::expected<net::RestRequest, BuildError> RequestBuilder::query_order(
    std::string_view symbol, const ExchangeClientOrderId& id, UnixNanos now) {
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NoCredentials};
    }
    Appender q{query_};
    q << "symbol=" << symbol << "&origClientOrderId=" << id.view();
    q << "&recvWindow=" << std::uint64_t{k_recv_window_ms} << "&timestamp=" << to_ms(now);
    if (!q.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return finish(net::HttpMethod::Get, ep::k_order, q.size());
}

// The account's own record of what it was charged — settlements AND commissions.
//
// `income` is SIGNED the way the account moved, so it needs no flipping to
// reach the domain's convention. REALIZED_PNL rows also arrive here and are
// dropped by the caller: that number is derived from fills this process already
// booked, and taking the venue's copy as well would count every close twice.
std::expected<net::RestRequest, BuildError> RequestBuilder::funding_income(UnixNanos since,
                                                                           UnixNanos now) {
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NoCredentials};
    }
    Appender q{query_};
    // NO incomeType FILTER. The same endpoint states COMMISSION beside
    // FUNDING_FEE, and the fee rows are what a fee reconciliation compares
    // against — asking twice would spend thirty weight twice for one answer.
    q << "limit=200";
    if (since.ns != 0) {
        q << "&startTime=" << to_ms(since);
    }
    q << "&recvWindow=" << std::uint64_t{k_recv_window_ms} << "&timestamp=" << to_ms(now);
    if (!q.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return finish(net::HttpMethod::Get, ep::k_income, q.size());
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::user_trades(std::string_view symbol, UnixNanos since, UnixNanos now) {
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NoCredentials};
    }
    Appender q{query_};
    q << "symbol=" << symbol;
    if (since.ns != 0) {
        q << "&startTime=" << to_ms(since);
    }
    q << "&recvWindow=" << std::uint64_t{k_recv_window_ms} << "&timestamp=" << to_ms(now);
    if (!q.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return finish(net::HttpMethod::Get, ep::k_user_trades, q.size());
}

std::expected<net::RestRequest, BuildError> RequestBuilder::position_risk(UnixNanos now) {
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NoCredentials};
    }
    Appender q{query_};
    q << "recvWindow=" << std::uint64_t{k_recv_window_ms} << "&timestamp=" << to_ms(now);
    if (!q.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return finish(net::HttpMethod::Get, ep::k_position_risk, q.size());
}

std::expected<net::RestRequest, BuildError> RequestBuilder::balance(UnixNanos now) {
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NoCredentials};
    }
    Appender q{query_};
    q << "recvWindow=" << std::uint64_t{k_recv_window_ms} << "&timestamp=" << to_ms(now);
    if (!q.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return finish(net::HttpMethod::Get, ep::k_balance, q.size());
}

std::expected<net::RestRequest, BuildError> RequestBuilder::listen_key_create() {
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NoCredentials};
    }
    return header_only(net::HttpMethod::Post, ep::k_listen_key);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::listen_key_keepalive() {
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NoCredentials};
    }
    return header_only(net::HttpMethod::Put, ep::k_listen_key);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::listen_key_close() {
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NoCredentials};
    }
    return header_only(net::HttpMethod::Delete, ep::k_listen_key);
}

} // namespace raikiri::venues::binance
