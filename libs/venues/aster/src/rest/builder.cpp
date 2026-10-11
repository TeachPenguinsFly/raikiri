#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/aster/endpoints.hpp>
#include <raikiri/venues/aster/rest.hpp>

#include <charconv>
#include <cstring>

namespace raikiri::venues::aster {

namespace {

constexpr std::string_view k_form = "application/x-www-form-urlencoded";

// tracks overflow; a truncated query would still be signed
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

// post-only is the GTX time in force
[[nodiscard]] std::string_view tif_text(TimeInForce tif, OrderFlags flags) noexcept {
    if (has_flag(flags, OrderFlags::PostOnly)) {
        return "GTX";
    }
    return tif == TimeInForce::Ioc ? "IOC" : "GTC";
}

[[nodiscard]] bool put_scaled(Appender& out, std::int64_t steps, FixedE9 step) noexcept {
    if (step.raw <= 0 || steps <= 0) {
        return false;
    }
    std::array<char, 40> buf{};
    const auto text = format_e9(FixedE9::from_raw(steps * step.raw), buf.data(), buf.size());
    if (text.empty()) {
        return false;
    }
    out << text;
    return true;
}

void put_target(Appender& q, const OrderCmd& cmd) noexcept {
    if (!cmd.target_exchange_order_id.empty()) {
        q << "&orderId=" << cmd.target_exchange_order_id.view();
    } else {
        q << "&origClientOrderId=" << cmd.exchange_client_order_id.view();
    }
}

void put_word(std::span<std::uint8_t> into, std::size_t at, const net::evm::Hash& value) noexcept {
    std::memcpy(into.data() + at, value.data(), value.size());
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

std::string_view rest_host(bool testnet, MarketType market) noexcept {
    return market == MarketType::Spot ? ep::k_spot_rest_live : ep::rest_host(testnet);
}

net::evm::Hash message_digest(std::string_view msg, std::uint64_t chain_id) noexcept {
    std::array<std::uint8_t, 5 * net::evm::k_hash_bytes> domain{};
    put_word(domain,
             0,
             net::evm::keccak256(std::string_view{
                 "EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)"}));
    put_word(domain, 32, net::evm::keccak256(std::string_view{"AsterSignTransaction"}));
    put_word(domain, 64, net::evm::keccak256(std::string_view{"1"}));
    for (std::size_t i = 0; i < 8; ++i) {
        domain[96 + 31 - i] = static_cast<std::uint8_t>(chain_id >> (8U * i));
    }
    // the verifying contract is address(0), so the last word stays zero
    std::array<std::uint8_t, 2 * net::evm::k_hash_bytes> message{};
    put_word(message, 0, net::evm::keccak256(std::string_view{"Message(string msg)"}));
    put_word(message, 32, net::evm::keccak256(msg));
    return net::evm::typed_digest(net::evm::keccak256(domain), net::evm::keccak256(message));
}

std::string_view sign_message(const net::evm::Wallet& wallet,
                              std::string_view msg,
                              std::uint64_t chain_id,
                              std::span<char> out) noexcept {
    net::evm::Signature sig{};
    if (!wallet.sign(message_digest(msg, chain_id), sig)) {
        return {};
    }
    return net::evm::rsv_hex(sig, out);
}

net::RestRequest RequestBuilder::instruments() noexcept {
    return {.method = net::HttpMethod::Get,
            .target = market_ == MarketType::Spot ? ep::k_spot_exchange_info : ep::k_exchange_info,
            .body = {}};
}

// one call returns every symbol
net::RestRequest RequestBuilder::ticker_24h() noexcept {
    return {.method = net::HttpMethod::Get,
            .target = market_ == MarketType::Spot ? ep::k_spot_ticker_24h : ep::k_ticker_24h,
            .body = {}};
}

net::RestRequest RequestBuilder::premium_index() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_premium_index, .body = {}};
}

net::RestRequest RequestBuilder::funding_info() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_funding_info, .body = {}};
}

net::RestRequest RequestBuilder::server_time() noexcept {
    return {.method = net::HttpMethod::Get,
            .target = market_ == MarketType::Spot ? ep::k_spot_server_time : ep::k_server_time,
            .body = {}};
}

net::RestRequest RequestBuilder::depth_snapshot(std::string_view symbol, std::uint32_t limit) noexcept {
    Appender target{target_};
    target << (market_ == MarketType::Spot ? ep::k_spot_depth : ep::k_depth) << "?symbol=" << symbol
           << "&limit=" << std::uint64_t{limit};
    return {.method = net::HttpMethod::Get, .target = target.ok() ? target.view() : std::string_view{}, .body = {}};
}

std::string_view RequestBuilder::last_sign_message() const noexcept {
    return {query_.data(), sign_len_};
}

std::uint64_t RequestBuilder::next_nonce(UnixNanos now) noexcept {
    const auto micros = now.ns / 1'000ULL;
    last_nonce_ = micros > last_nonce_ ? micros : last_nonce_ + 1;
    return last_nonce_;
}

// the parameters are in query_; the nonce, user and signer go after them, then the signature
std::expected<net::RestRequest, BuildError>
RequestBuilder::finish(net::HttpMethod method, std::string_view path, std::size_t query_len, UnixNanos now) {
    if (!has_credentials()) {
        return std::unexpected{BuildError::NoCredentials};
    }
    if (query_len > query_.size()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    Appender auth{std::span{query_}.subspan(query_len)};
    auth << (query_len == 0 ? "" : "&") << "nonce=" << next_nonce(now) << "&user=" << signer_->api_key()
         << "&signer=" << wallet_->address();
    if (!auth.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    sign_len_ = query_len + auth.size();
    const std::string_view query{query_.data(), sign_len_};
    const auto signature = sign_message(*wallet_, query, chain_id_, signature_);
    if (signature.empty()) {
        return std::unexpected{BuildError::NoCredentials};
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
    return request;
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::create_order(const OrderCmd& cmd, const InstrumentBase& instrument, UnixNanos now) {
    if (cmd.order_type == OrderType::Market && has_flag(cmd.flags, OrderFlags::PostOnly)) {
        return std::unexpected{BuildError::UnsupportedOrderType};
    }
    // aster lists GTC, IOC, GTX and HIDDEN; a FOK is refused here rather than by the venue
    if (cmd.order_type == OrderType::Limit && cmd.tif == TimeInForce::Fok) {
        return std::unexpected{BuildError::UnsupportedOrderType};
    }
    Appender q{query_};
    q << "symbol=" << instrument.exchange_symbol.view() << "&side=" << side_text(cmd.side)
      << "&type=" << (cmd.order_type == OrderType::Market ? "MARKET" : "LIMIT");
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
    if (!q.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return finish(net::HttpMethod::Post, ep::k_order, q.size(), now);
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::cancel_order(const OrderCmd& cmd, const InstrumentBase& instrument, UnixNanos now) {
    Appender q{query_};
    q << "symbol=" << instrument.exchange_symbol.view();
    put_target(q, cmd);
    if (!q.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return finish(net::HttpMethod::Delete, ep::k_order, q.size(), now);
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::amend_order(const OrderCmd& cmd, const InstrumentBase& instrument, UnixNanos now) {
    Appender q{query_};
    q << "symbol=" << instrument.exchange_symbol.view() << "&side=" << side_text(cmd.side);
    put_target(q, cmd);
    q << "&quantity=";
    if (!put_scaled(q, cmd.qty.raw, instrument.lot_size)) {
        return std::unexpected{BuildError::BadValue};
    }
    q << "&price=";
    if (!put_scaled(q, cmd.price.raw, instrument.tick_size)) {
        return std::unexpected{BuildError::BadValue};
    }
    if (!q.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return finish(net::HttpMethod::Put, ep::k_order, q.size(), now);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::open_orders(UnixNanos now) {
    return finish(net::HttpMethod::Get, ep::k_open_orders, 0, now);
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::query_order(std::string_view symbol, const ExchangeClientOrderId& id, UnixNanos now) {
    Appender q{query_};
    q << "symbol=" << symbol << "&origClientOrderId=" << id.view();
    if (!q.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return finish(net::HttpMethod::Get, ep::k_order, q.size(), now);
}

// an empty symbol asks for the whole account
std::expected<net::RestRequest, BuildError>
RequestBuilder::user_trades(std::string_view symbol, UnixNanos since, UnixNanos now) {
    Appender q{query_};
    q << "limit=1000";
    if (!symbol.empty()) {
        q << "&symbol=" << symbol;
    }
    if (since.ns != 0) {
        q << "&startTime=" << since.ns / 1'000'000ULL;
    }
    if (!q.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return finish(net::HttpMethod::Get, ep::k_user_trades, q.size(), now);
}

// no incomeType filter: fee rows are wanted beside the funding ones
std::expected<net::RestRequest, BuildError> RequestBuilder::funding_income(UnixNanos since, UnixNanos now) {
    Appender q{query_};
    q << "limit=200";
    if (since.ns != 0) {
        q << "&startTime=" << since.ns / 1'000'000ULL;
    }
    if (!q.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return finish(net::HttpMethod::Get, ep::k_income, q.size(), now);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::position_risk(UnixNanos now) {
    return finish(net::HttpMethod::Get, ep::k_position_risk, 0, now);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::account(UnixNanos now) {
    return finish(net::HttpMethod::Get, ep::k_account, 0, now);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::listen_key_create(UnixNanos now) {
    return finish(net::HttpMethod::Post, ep::k_listen_key, 0, now);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::listen_key_keepalive(UnixNanos now) {
    return finish(net::HttpMethod::Put, ep::k_listen_key, 0, now);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::listen_key_close(UnixNanos now) {
    return finish(net::HttpMethod::Delete, ep::k_listen_key, 0, now);
}

} // namespace raikiri::venues::aster
