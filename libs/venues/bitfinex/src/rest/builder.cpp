#include <raikiri/config/secret.hpp>
#include <raikiri/net/json_writer.hpp>
#include <raikiri/venues/bitfinex/orders.hpp>
#include <raikiri/venues/bitfinex/rest.hpp>

#include <charconv>
#include <cstring>
#include <optional>
#include <span>

namespace raikiri::venues::bitfinex {

namespace {

// the public host; trading uses api.bitfinex.com
constexpr std::string_view k_host = "api-pub.bitfinex.com";
constexpr std::string_view k_private_host = "api.bitfinex.com";

constexpr std::string_view k_hdr_nonce = "bfx-nonce";
constexpr std::string_view k_hdr_apikey = "bfx-apikey";
constexpr std::string_view k_hdr_signature = "bfx-signature";

// signed with the /api prefix, sent without it
constexpr std::string_view k_sign_prefix = "/api";

// an empty payload is "{}"; an empty body fails the signature
constexpr std::string_view k_empty_body = "{}";

constexpr std::string_view k_ulid_alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr std::size_t k_ulid_len = 26;

// the paged reads allow up to 2500
constexpr std::int64_t k_page_limit = 500;

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

// builders share one buffer; send before building the next
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

namespace {

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

// a sell is a negative amount; there is no side field
[[nodiscard]] std::optional<FixedE9> signed_amount(const OrderCmd& cmd, const InstrumentBase& base) noexcept {
    const auto qty = base.lots_to_qty(cmd.qty);
    if (qty.is_undef() || qty.raw <= 0) {
        return std::nullopt;
    }
    return cmd.side == Side::Buy ? qty : FixedE9::from_raw(-qty.raw);
}

[[nodiscard]] std::optional<FixedE9> order_price(const OrderCmd& cmd, const InstrumentBase& base) noexcept {
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
        // bitfinex has no dated contracts
        return BuildError::DatedFuturesUnsupported;
    case MarketType::Option:
        return BuildError::OptionsUnsupported;
    }
    return BuildError::OptionsUnsupported;
}

std::string_view order_type(MarketType market, OrderType type, TimeInForce tif) noexcept {
    // without the "EXCHANGE " prefix the order opens a margin position
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
    // keeps the low 48 bits, all from the ulid's random part
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

    // /api + path + nonce + body with no separator; the prefix is signed but not sent
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
    request.headers = {{k_hdr_nonce, nonce.view()}, {k_hdr_apikey, api_key_}, {k_hdr_signature, signature}};
    return request;
}

std::expected<net::RestRequest, BuildError>
SignedRequestBuilder::create_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
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
    // decimals are quoted to keep the exact spelling
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

std::expected<net::RestRequest, BuildError>
SignedRequestBuilder::cancel_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
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
    // id is a number, so it goes in unquoted
    json.obj_begin().raw("id", venue_id).obj_end();
    if (!json.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign("/v2/auth/w/order/cancel", json.view(), now);
}

std::expected<net::RestRequest, BuildError>
SignedRequestBuilder::amend_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
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
    // the amend restates the signed amount
    json.obj_begin().raw("id", venue_id).decimal("amount", *amount).decimal("price", *price).obj_end();
    if (!json.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign("/v2/auth/w/order/update", json.view(), now);
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::open_orders(MarketType market, UnixNanos now) {
    if (const auto refusal = market_refusal(market); refusal.has_value()) {
        return std::unexpected{*refusal};
    }
    // account-wide; the mapper separates the markets
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

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::positions(MarketType market, UnixNanos now) {
    switch (market) {
    case MarketType::Perp:
        return sign("/v2/auth/r/positions", k_empty_body, now);
    case MarketType::Spot:
        return std::unexpected{BuildError::SpotPositionsUnsupported};
    case MarketType::Future:
        return std::unexpected{BuildError::DatedFuturesUnsupported};
    case MarketType::Option:
        return std::unexpected{BuildError::OptionsUnsupported};
    }
    return std::unexpected{BuildError::UnknownInstrument};
}

} // namespace raikiri::venues::bitfinex
