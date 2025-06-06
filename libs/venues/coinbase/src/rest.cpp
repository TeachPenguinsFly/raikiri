#include <raikiri/net/json_writer.hpp>
#include <raikiri/venues/coinbase/rest.hpp>

#include <charconv>
#include <cstring>
#include <span>

namespace raikiri::venues::coinbase {

namespace {

constexpr std::string_view k_host = "api.exchange.coinbase.com";

constexpr std::string_view k_hdr_key = "CB-ACCESS-KEY";
constexpr std::string_view k_hdr_sign = "CB-ACCESS-SIGN";
constexpr std::string_view k_hdr_timestamp = "CB-ACCESS-TIMESTAMP";
constexpr std::string_view k_hdr_passphrase = "CB-ACCESS-PASSPHRASE";

// Crockford base32, the ULID alphabet: no I, L, O or U.
constexpr std::string_view k_crockford = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr std::string_view k_hex = "0123456789abcdef";

constexpr std::size_t k_ulid_len = 26;
constexpr std::size_t k_uuid_bytes = 16;

// Fills the builder's target buffer. A part that would overflow is dropped
// rather than truncated mid-token, so an over-long product id yields a
// request Coinbase answers with 404 NotFound instead of one that names a
// different product.
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

net::RestRequest RequestBuilder::products() noexcept {
    return {.method = net::HttpMethod::Get, .target = "/products", .body = {}};
}

net::RestRequest RequestBuilder::server_time() noexcept {
    return {.method = net::HttpMethod::Get, .target = "/time", .body = {}};
}

// Every builder below writes into the same buffer, so a request must be sent
// before the next one is built. One catalogue load at a time is the only use.
net::RestRequest RequestBuilder::product(std::string_view product_id) noexcept {
    TargetWriter writer{target_};
    writer.put("/products/");
    writer.put(product_id);
    return writer.get();
}

net::RestRequest RequestBuilder::book(std::string_view product_id, std::uint32_t level) noexcept {
    TargetWriter writer{target_};
    writer.put("/products/");
    writer.put(product_id);
    writer.put("/book?level=");
    writer.put(level);
    return writer.get();
}


namespace {

// Reports overflow instead of truncating: a signed body cut mid-number
// authenticates an order nobody placed.
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

    Appender& operator<<(std::uint32_t value) noexcept {
        std::array<char, 16> num{};
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

[[nodiscard]] std::string_view tif_text(TimeInForce tif) noexcept {
    switch (tif) {
    case TimeInForce::Gtc:
        return "GTC";
    case TimeInForce::Ioc:
        return "IOC";
    case TimeInForce::Fok:
        return "FOK";
    }
    return "GTC";
}

} // namespace

std::optional<std::array<char, k_uuid_len>>
client_oid_of(const ExchangeClientOrderId& id) noexcept {
    const auto text = id.view();
    if (text.size() != k_ulid_len) {
        return std::nullopt;
    }
    // 26 Crockford characters carry 130 bits; a valid ULID leaves the top two
    // clear, so the low 128 are the whole value.
    std::array<std::uint8_t, k_uuid_bytes> bytes{};
    std::uint32_t accumulator = 0;
    int bits = 0;
    std::size_t written = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto at = k_crockford.find(text[i]);
        if (at == std::string_view::npos) {
            return std::nullopt;
        }
        const auto value = static_cast<std::uint32_t>(at);
        if (i == 0 && value > 7) {
            return std::nullopt; // past 128 bits: not a ULID this codebase minted
        }
        const int width = i == 0 ? 3 : 5;
        accumulator = (accumulator << static_cast<unsigned>(width)) |
                      (value & ((1U << static_cast<unsigned>(width)) - 1U));
        bits += width;
        while (bits >= 8) {
            bits -= 8;
            if (written == bytes.size()) {
                return std::nullopt;
            }
            bytes[written++] = static_cast<std::uint8_t>((accumulator >> bits) & 0xFFU);
        }
    }
    if (written != bytes.size() || bits != 0) {
        return std::nullopt;
    }
    // Version 4 and variant 1, which is what Coinbase means by "variant 1
    // UUIDv4". Both nibbles land in the ULID's RANDOM half — the timestamp is
    // the first six bytes — so what is overwritten is entropy, not identity.
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);

    std::array<char, k_uuid_len> out{};
    std::size_t at = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out[at++] = '-';
        }
        out[at++] = k_hex[(bytes[i] >> 4U) & 0x0FU];
        out[at++] = k_hex[bytes[i] & 0x0FU];
    }
    return out;
}

std::string_view to_string(BuildError err) noexcept {
    switch (err) {
    case BuildError::UnknownInstrument:
        return "UnknownInstrument";
    case BuildError::BadQuantity:
        return "BadQuantity";
    case BuildError::BadPrice:
        return "BadPrice";
    case BuildError::BufferTooSmall:
        return "BufferTooSmall";
    case BuildError::NotSigned:
        return "NotSigned";
    case BuildError::NoOrderTarget:
        return "NoOrderTarget";
    case BuildError::ClientOrderIdNotAUlid:
        return "ClientOrderIdNotAUlid";
    case BuildError::PerpNotListed:
        return "PerpNotListed";
    case BuildError::DatedFuturesNotListed:
        return "DatedFuturesNotListed";
    case BuildError::OptionsNotListed:
        return "OptionsNotListed";
    case BuildError::AmendUnsupported:
        return "AmendUnsupported";
    }
    return "Unknown";
}

std::optional<BuildError> market_refusal(MarketType market) noexcept {
    switch (market) {
    case MarketType::Spot:
        return std::nullopt;
    case MarketType::Perp:
        // Coinbase does list perpetuals — on Coinbase International, a
        // different entity, a different host and a different API. Routing one
        // here would silently place a SPOT order for the same base currency.
        return BuildError::PerpNotListed;
    case MarketType::Future:
        return BuildError::DatedFuturesNotListed;
    case MarketType::Option:
        return BuildError::OptionsNotListed;
    }
    return BuildError::OptionsNotListed;
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::sign(net::HttpMethod method,
                                                                       std::string_view target,
                                                                       std::string_view body,
                                                                       UnixNanos now) {
    if (target.empty()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    if (!signer_->has_credentials() || passphrase_.empty()) {
        // The passphrase is a third credential, unlike every venue here but
        // okx and kucoin, and a request missing it is refused 401 in a way
        // that reads exactly like a wrong secret.
        return std::unexpected{BuildError::NotSigned};
    }
    const auto timestamp = timestamp_s(now, timestamp_);
    // The requestPath signed is the target INCLUDING its query string, which
    // is why signing happens after the target is complete rather than beside
    // the path that opened it.
    const auto message = prehash(timestamp, method, target, body, prehash_);
    if (timestamp.empty() || message.empty()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    prehash_len_ = message.size();
    // Base64 of the digest, keyed by the already-decoded secret; see sign.hpp
    // for why the decode does not happen here.
    const auto signature = signer_->sign(message, net::Signer::Encoding::Base64, signature_);
    if (signature.empty()) {
        return std::unexpected{BuildError::NotSigned};
    }

    net::RestRequest request{};
    request.method = method;
    request.target = target;
    request.body = body;
    request.headers = {{k_hdr_key, signer_->api_key()},
                       {k_hdr_sign, signature},
                       {k_hdr_timestamp, timestamp},
                       {k_hdr_passphrase, passphrase_}};
    return request;
}

std::string_view SignedRequestBuilder::last_prehash() const noexcept {
    return {prehash_.data(), prehash_len_};
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
    const auto client_oid = client_oid_of(cmd.exchange_client_order_id);
    if (!client_oid.has_value()) {
        return std::unexpected{BuildError::ClientOrderIdNotAUlid};
    }
    if (cmd.qty.is_undef() || cmd.qty.raw <= 0) {
        return std::unexpected{BuildError::BadQuantity};
    }
    const auto size = base.lots_to_qty(cmd.qty);
    if (size.is_undef() || size.raw <= 0) {
        return std::unexpected{BuildError::BadQuantity};
    }
    const bool is_market = cmd.order_type == OrderType::Market;

    net::JsonWriter json{body_};
    json.obj_begin()
        .str("client_oid", std::string_view{client_oid->data(), client_oid->size()})
        .str("product_id", base.exchange_symbol.view())
        .str("side", cmd.side == Side::Sell ? "sell" : "buy")
        .str("type", is_market ? "market" : "limit")
        // `size` is BASE currency on both sides, so unlike gate and htx a
        // market BUY needs no quote-denominated guess and is not refused.
        .decimal("size", size);
    if (!is_market) {
        const auto price = base.ticks_to_price(cmd.price);
        if (price.is_undef() || price.raw <= 0) {
            return std::unexpected{BuildError::BadPrice};
        }
        json.decimal("price", price).str("time_in_force", tif_text(cmd.tif));
        // post_only is INVALID beside IOC or FOK — the request is refused, not
        // ignored — so a maker-only order that is not GTC simply is not one.
        if (has_flag(cmd.flags, OrderFlags::PostOnly) && cmd.tif == TimeInForce::Gtc) {
            json.boolean("post_only", true);
        }
    }
    // ReduceOnly is deliberately not forwarded: a spot balance cannot be
    // short, and Coinbase Exchange has no field for it.
    json.obj_end();
    if (!json.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Post, "/orders", json.view(), now);
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::cancel_order(
    const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
    if (const auto refusal = market_refusal(cmd.instrument.market()); refusal.has_value()) {
        return std::unexpected{*refusal};
    }
    Appender target{target_};
    target << "/orders/";
    if (!cmd.target_exchange_order_id.empty()) {
        target << cmd.target_exchange_order_id.view();
    } else {
        const auto client_oid = client_oid_of(cmd.exchange_client_order_id);
        if (!client_oid.has_value()) {
            return std::unexpected{cmd.exchange_client_order_id.empty()
                                       ? BuildError::NoOrderTarget
                                       : BuildError::ClientOrderIdNotAUlid};
        }
        // The `client:` namespace is what tells Coinbase the id is ours; the
        // bare form is read as a venue id and answers 404.
        target << "client:" << std::string_view{client_oid->data(), client_oid->size()};
    }
    // Naming the product is optional and cheaper for the venue, so it is sent
    // whenever the table can supply it.
    if (const auto symbol = table.exchange_symbol(cmd.instrument); !symbol.empty()) {
        target << "?product_id=" << symbol;
    }
    if (!target.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Delete, target.view(), {}, now);
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::amend_order(
    const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
    (void)table;
    (void)now;
    if (const auto refusal = market_refusal(cmd.instrument.market()); refusal.has_value()) {
        return std::unexpected{*refusal};
    }
    return std::unexpected{BuildError::AmendUnsupported};
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::open_orders(MarketType market,
                                                                              UnixNanos now) {
    if (const auto refusal = market_refusal(market); refusal.has_value()) {
        return std::unexpected{*refusal};
    }
    Appender target{target_};
    // Both statuses on purpose: `pending` is an order the matching engine has
    // taken but not yet placed, and leaving it out reports a live order as
    // absent for as long as that lasts.
    target << "/orders?status=open&status=pending&limit=" << k_max_page;
    if (!target.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Get, target.view(), {}, now);
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::query_order(
    const ExchangeClientOrderId& id, MarketType market, UnixNanos now) {
    if (const auto refusal = market_refusal(market); refusal.has_value()) {
        return std::unexpected{*refusal};
    }
    const auto client_oid = client_oid_of(id);
    if (!client_oid.has_value()) {
        return std::unexpected{id.empty() ? BuildError::NoOrderTarget
                                          : BuildError::ClientOrderIdNotAUlid};
    }
    Appender target{target_};
    target << "/orders/client:" << std::string_view{client_oid->data(), client_oid->size()};
    if (!target.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Get, target.view(), {}, now);
}

} // namespace raikiri::venues::coinbase
