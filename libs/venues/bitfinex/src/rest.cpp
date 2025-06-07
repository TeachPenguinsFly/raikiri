#include <raikiri/config/secret.hpp>
#include <raikiri/net/json_writer.hpp>
#include <raikiri/venues/bitfinex/rest.hpp>

#include <charconv>
#include <cstring>
#include <optional>
#include <span>

namespace raikiri::venues::bitfinex {

namespace {

// The public host. Bitfinex splits reads from trading (api.bitfinex.com), and
// only the read side is used here.
constexpr std::string_view k_host = "api-pub.bitfinex.com";
constexpr std::string_view k_private_host = "api.bitfinex.com";

// Header names are Bitfinex's, lowercase and all.
constexpr std::string_view k_hdr_nonce = "bfx-nonce";
constexpr std::string_view k_hdr_apikey = "bfx-apikey";
constexpr std::string_view k_hdr_signature = "bfx-signature";

// The digest covers the path with this prefix, which is NOT what goes on the
// wire — the request target is `/v2/...` and the signed string `/api/v2/...`.
constexpr std::string_view k_sign_prefix = "/api";

// An empty payload is two bytes, not zero: the SDK signs and sends
// JSON.stringify({}) and a body of "" produces a signature Bitfinex refuses.
constexpr std::string_view k_empty_body = "{}";

// Crockford base32, the ULID alphabet.
constexpr std::string_view k_ulid_alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr std::size_t k_ulid_len = 26;

// The paged reads cap at 2500; a reconciliation sweep wants the whole picture
// in one round trip rather than a paginated approximation of it.
constexpr std::int64_t k_page_limit = 500;

// Fills the builder's target buffer. A part that would overflow is dropped
// rather than truncated mid-token, so an over-long symbol yields a request
// Bitfinex refuses with 10020 instead of one that names a different pair.
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

} // namespace

std::string_view rest_host() noexcept {
    return k_host;
}

net::RestRequest RequestBuilder::platform_status() noexcept {
    return {.method = net::HttpMethod::Get, .target = "/v2/platform/status", .body = {}};
}

net::RestRequest RequestBuilder::tickers() noexcept {
    return {.method = net::HttpMethod::Get, .target = "/v2/tickers?symbols=ALL", .body = {}};
}

// Every builder below writes into the same buffer, so a request must be sent
// before the next one is built. One catalogue load at a time is the only use.
net::RestRequest RequestBuilder::pair_info(bool futures) noexcept {
    TargetWriter writer{target_};
    writer.put("/v2/conf/pub:info:pair");
    if (futures) {
        writer.put(":futures");
    }
    return writer.get();
}

net::RestRequest RequestBuilder::book(std::string_view symbol, std::uint32_t len) noexcept {
    TargetWriter writer{target_};
    writer.put("/v2/book/");
    writer.put(symbol);
    writer.put("/P0?len=");
    writer.put(len);
    return writer.get();
}

// ---------------------------------------------------------- private ----

namespace {

// Appends into a fixed buffer, reporting overflow rather than truncating: a
// truncated signed payload authenticates an order that is not the one intended.
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

// SIGNED, AND THE SIGN IS THE SIDE. A sell is a negative amount and there is
// no side field beside it: sending a positive amount for a sell BUYS.
[[nodiscard]] std::optional<FixedE9> signed_amount(const OrderCmd& cmd,
                                                   const InstrumentBase& base) noexcept {
    const auto qty = base.lots_to_qty(cmd.qty);
    if (qty.is_undef() || qty.raw <= 0) {
        return std::nullopt;
    }
    return cmd.side == Side::Buy ? qty : FixedE9::from_raw(-qty.raw);
}

[[nodiscard]] std::optional<FixedE9> order_price(const OrderCmd& cmd,
                                                 const InstrumentBase& base) noexcept {
    const auto price = base.ticks_to_price(cmd.price);
    if (price.is_undef() || price.raw <= 0) {
        return std::nullopt;
    }
    return price;
}

} // namespace

std::string_view private_rest_host() noexcept {
    return k_private_host;
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
        return "bitfinex cancels and amends need the venue order id";
    case BuildError::ClientIdNotDerivable:
        return "client order id is not a ULID";
    case BuildError::OptionsUnsupported:
        return "bitfinex options order entry";
    case BuildError::DatedFuturesUnsupported:
        return "bitfinex dated futures order entry";
    case BuildError::SpotPositionsUnsupported:
        return "bitfinex spot position pull";
    }
    return "unknown";
}

std::optional<BuildError> market_refusal(MarketType market) noexcept {
    switch (market) {
    case MarketType::Spot:
    case MarketType::Perp:
        return std::nullopt;
    case MarketType::Future:
        // /conf/pub:info:pair:futures lists perpetuals only; Bitfinex has no
        // dated contract, so no id for one is ever loaded.
        return BuildError::DatedFuturesUnsupported;
    case MarketType::Option:
        return BuildError::OptionsUnsupported;
    }
    return BuildError::OptionsUnsupported;
}

std::string_view order_type(MarketType market, OrderType type, TimeInForce tif) noexcept {
    // The "EXCHANGE " prefix is what makes an order a SPOT order. Without it
    // the same request opens a margin position — the venue accepts it, so the
    // mistake shows up as an unexpected position rather than as an error.
    const bool spot = market == MarketType::Spot;
    if (type == OrderType::Market) {
        return spot ? "EXCHANGE MARKET" : "MARKET";
    }
    switch (tif) {
    case TimeInForce::Ioc:
        return spot ? "EXCHANGE IOC" : "IOC";
    case TimeInForce::Fok:
        return spot ? "EXCHANGE FOK" : "FOK";
    case TimeInForce::Gtc:
        break;
    }
    return spot ? "EXCHANGE LIMIT" : "LIMIT";
}

std::int64_t order_flags(OrderFlags flags) noexcept {
    std::int64_t out = 0;
    if (has_flag(flags, OrderFlags::PostOnly)) {
        out |= k_flag_post_only;
    }
    if (has_flag(flags, OrderFlags::ReduceOnly)) {
        out |= k_flag_reduce_only;
    }
    return out;
}

std::optional<std::int64_t> cid_from(const ExchangeClientOrderId& id) noexcept {
    const auto text = id.view();
    if (text.size() != k_ulid_len) {
        return std::nullopt;
    }
    // The low 48 bits of the ULID's 128 — 48 of its 80 random bits, so
    // uniqueness is inherited rather than hashed. Accumulating in uint64 keeps
    // exactly those bits and discards the timestamp half, which is what the
    // day-scoped cid space has room for.
    std::uint64_t low = 0;
    for (const char c : text) {
        const auto at = k_ulid_alphabet.find(c);
        if (at == std::string_view::npos) {
            return std::nullopt;
        }
        low = (low << 5) | static_cast<std::uint64_t>(at);
    }
    const auto mask = (std::uint64_t{1} << k_cid_bits) - 1;
    return static_cast<std::int64_t>(low & mask);
}

std::string_view SignedRequestBuilder::last_sign_message() const noexcept {
    return {sign_msg_.data(), sign_msg_len_};
}

std::expected<net::RestRequest, BuildError>
SignedRequestBuilder::sign(std::string_view path, std::string_view body, UnixNanos now) {
    if (api_key_.empty() || secret_ == nullptr || secret_->empty()) {
        return std::unexpected{BuildError::NotSigned};
    }
    Appender nonce{nonce_};
    nonce << static_cast<std::int64_t>(nonces_.next(now.ns));
    if (!nonce.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }

    // `/api` + path + nonce + body, concatenated with no separator at all.
    // The prefix is signed and NOT sent; the body is signed exactly as sent.
    Appender msg{sign_msg_};
    msg << k_sign_prefix << path << nonce.view() << body;
    if (!msg.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    sign_msg_len_ = msg.view().size();

    const auto signature = hmac_sha384_hex(secret_->view(), msg.view(), signature_);
    if (signature.empty()) {
        return std::unexpected{BuildError::NotSigned};
    }

    net::RestRequest request{};
    request.method = net::HttpMethod::Post;
    request.target = path;
    request.body = body;
    request.headers = {{k_hdr_nonce, nonce.view()},
                       {k_hdr_apikey, api_key_},
                       {k_hdr_signature, signature}};
    return request;
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::create_order(
    const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
    const auto market = cmd.instrument.market();
    if (const auto refusal = market_refusal(market); refusal.has_value()) {
        return std::unexpected{*refusal};
    }
    const auto* instrument = table.find(cmd.instrument);
    if (instrument == nullptr) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    const auto& base = base_of(*instrument);
    const auto cid = cid_from(cmd.exchange_client_order_id);
    if (!cid.has_value()) {
        return std::unexpected{BuildError::ClientIdNotDerivable};
    }
    const auto amount = signed_amount(cmd, base);
    if (!amount.has_value()) {
        return std::unexpected{BuildError::BadQuantity};
    }

    net::JsonWriter body{body_};
    // Decimals are QUOTED. Bitfinex takes them either way, and a string keeps
    // the exact fixed-point spelling out of any client's double.
    body.obj_begin()
        .str("type", order_type(market, cmd.order_type, cmd.tif))
        .str("symbol", base.exchange_symbol.view())
        .decimal("amount", *amount);
    if (cmd.order_type != OrderType::Market) {
        const auto price = order_price(cmd, base);
        if (!price.has_value()) {
            return std::unexpected{BuildError::BadPrice};
        }
        body.decimal("price", *price);
    }
    body.num("cid", *cid).num("flags", order_flags(cmd.flags)).obj_end();
    if (!body.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign("/v2/auth/w/order/submit", body.view(), now);
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::cancel_order(
    const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
    if (const auto refusal = market_refusal(cmd.instrument.market()); refusal.has_value()) {
        return std::unexpected{*refusal};
    }
    if (table.find(cmd.instrument) == nullptr) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    const auto venue_id = cmd.target_exchange_order_id.view();
    if (!all_digits(venue_id)) {
        return std::unexpected{BuildError::NoOrderTarget};
    }
    net::JsonWriter json{body_};
    // `id` is declared a number and Bitfinex order ids are digit runs, so it
    // goes in UNQUOTED.
    json.obj_begin().raw("id", venue_id).obj_end();
    if (!json.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign("/v2/auth/w/order/cancel", json.view(), now);
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
    const auto venue_id = cmd.target_exchange_order_id.view();
    if (!all_digits(venue_id)) {
        return std::unexpected{BuildError::NoOrderTarget};
    }
    const auto& base = base_of(*instrument);
    const auto amount = signed_amount(cmd, base);
    const auto price = order_price(cmd, base);
    if (!amount.has_value()) {
        return std::unexpected{BuildError::BadQuantity};
    }
    if (!price.has_value()) {
        return std::unexpected{BuildError::BadPrice};
    }
    net::JsonWriter json{body_};
    // The amend restates the SIGNED amount, so an amend that forgot the sign
    // would flip a resting sell into a buy at the new price.
    json.obj_begin()
        .raw("id", venue_id)
        .decimal("amount", *amount)
        .decimal("price", *price)
        .obj_end();
    if (!json.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign("/v2/auth/w/order/update", json.view(), now);
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::open_orders(MarketType market,
                                                                              UnixNanos now) {
    if (const auto refusal = market_refusal(market); refusal.has_value()) {
        return std::unexpected{*refusal};
    }
    // /auth/r/orders is account-wide and names no market, so both markets come
    // back on one call and the mapper is what separates them.
    return sign("/v2/auth/r/orders", k_empty_body, now);
}

std::expected<net::RestRequest, BuildError>
SignedRequestBuilder::my_trades(MarketType market, UnixNanos since, UnixNanos now) {
    if (const auto refusal = market_refusal(market); refusal.has_value()) {
        return std::unexpected{*refusal};
    }
    const auto since_ms = static_cast<std::int64_t>(since.ns / 1'000'000ULL);
    net::JsonWriter json{body_};
    json.obj_begin();
    if (since_ms > 0) {
        json.num("start", since_ms);
    }
    json.num("limit", k_page_limit).obj_end();
    if (!json.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign("/v2/auth/r/trades/hist", json.view(), now);
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::positions(MarketType market,
                                                                            UnixNanos now) {
    switch (market) {
    case MarketType::Perp:
        return sign("/v2/auth/r/positions", k_empty_body, now);
    case MarketType::Spot:
        // A spot balance is not a position. Answering "flat" would be a claim
        // about a concept this API does not have.
        return std::unexpected{BuildError::SpotPositionsUnsupported};
    case MarketType::Future:
        return std::unexpected{BuildError::DatedFuturesUnsupported};
    case MarketType::Option:
        return std::unexpected{BuildError::OptionsUnsupported};
    }
    return std::unexpected{BuildError::UnknownInstrument};
}

} // namespace raikiri::venues::bitfinex
