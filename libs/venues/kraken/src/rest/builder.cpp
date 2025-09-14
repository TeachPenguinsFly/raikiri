#include <raikiri/venues/kraken/endpoints.hpp>
#include <raikiri/venues/kraken/rest.hpp>

#include <charconv>
#include <cstring>
#include <span>

namespace raikiri::venues::kraken {

namespace {

constexpr std::string_view k_hdr_key = "APIKey";
constexpr std::string_view k_hdr_nonce = "Nonce";
constexpr std::string_view k_hdr_authent = "Authent";
constexpr std::string_view k_form_content_type = "application/x-www-form-urlencoded";

// Fills the builder's target buffer. An overflowing part is dropped rather
// than truncated mid-token, so an over-long symbol yields a request Kraken
// refuses instead of one naming a different instrument.
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
// truncated parameter list is signed as faithfully as a complete one, so the
// order that arrives is not the order that was authorised.
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

    Appender& operator<<(FixedE9 value) noexcept {
        std::array<char, 40> num{};
        const auto text = format_e9(value, num.data(), num.size());
        if (text.empty()) {
            overflow_ = true;
            return *this;
        }
        return *this << text;
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

[[nodiscard]] std::string_view render_u64(std::uint64_t value, std::span<char> out) noexcept {
    const auto [stop, ec] = std::to_chars(out.data(), out.data() + out.size(), value);
    return ec == std::errc{}
               ? std::string_view{out.data(), static_cast<std::size_t>(stop - out.data())}
               : std::string_view{};
}

// Kraken names the order type and the time in force in ONE field. Post-only is
// read before the time in force because "post" and "ioc" are alternatives here
// rather than a flag beside one: an order asking for both cannot be sent, and
// resting is the intent post-only expresses.
[[nodiscard]] std::string_view order_type_text(const OrderCmd& cmd) noexcept {
    if (cmd.order_type == OrderType::Market) {
        // A market order never rests, so no time in force distinguishes one
        // "mkt" from another.
        return "mkt";
    }
    if (has_flag(cmd.flags, OrderFlags::PostOnly)) {
        return "post";
    }
    switch (cmd.tif) {
    case TimeInForce::Ioc:
        return "ioc";
    case TimeInForce::Fok:
        return "fok";
    case TimeInForce::Gtc:
        break;
    }
    return "lmt";
}

[[nodiscard]] std::optional<FixedE9> order_qty(const OrderCmd& cmd,
                                               const InstrumentBase& base) noexcept {
    const auto qty = base.lots_to_qty(cmd.qty);
    if (qty.is_undef() || qty.raw <= 0) {
        return std::nullopt;
    }
    return qty;
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
    case BuildError::SpotUnsupported:
        return "kraken spot order entry";
    case BuildError::DatedFuturesUnsupported:
        return "kraken dated futures order entry";
    case BuildError::OptionsUnsupported:
        return "kraken options order entry";
    }
    return "unknown";
}

// This adapter serves the perpetual futures — PF_ (linear) and PI_ (inverse) —
// and nothing else. Each of the other three markets gets its OWN name so a
// refusal says which surface is missing rather than blaming the catalogue.
std::optional<BuildError> market_refusal(MarketType market) noexcept {
    switch (market) {
    case MarketType::Perp:
        return std::nullopt;
    case MarketType::Spot:
        return BuildError::SpotUnsupported;
    case MarketType::Future:
        return BuildError::DatedFuturesUnsupported;
    case MarketType::Option:
        return BuildError::OptionsUnsupported;
    }
    return BuildError::OptionsUnsupported;
}

// Every builder writes the same buffer, so a request must be sent before the
// next is built. One catalogue load at a time is the only use.
net::RestRequest RequestBuilder::instruments() noexcept {
    TargetWriter writer{target_};
    writer.put(ep::k_route_prefix);
    writer.put(ep::k_instruments);
    return writer.get();
}

net::RestRequest RequestBuilder::ticker(std::string_view symbol) noexcept {
    TargetWriter writer{target_};
    writer.put(ep::k_route_prefix);
    writer.put(ep::k_tickers);
    writer.put("/");
    writer.put(symbol);
    return writer.get();
}

net::RestRequest RequestBuilder::orderbook(std::string_view symbol) noexcept {
    TargetWriter writer{target_};
    writer.put(ep::k_route_prefix);
    writer.put(ep::k_orderbook);
    writer.put("?symbol=");
    writer.put(symbol);
    return writer.get();
}

// -------------------------------------------------------------- signed ----

std::string_view SignedRequestBuilder::last_sign_message() const noexcept {
    return {sign_msg_.data(), sign_msg_len_};
}

std::string_view SignedRequestBuilder::last_authent() const noexcept {
    return {authent_.data(), authent_len_};
}

std::string_view SignedRequestBuilder::next_nonce(UnixNanos now) noexcept {
    auto value = now.ns / 1'000'000ULL;
    if (value <= last_nonce_) {
        value = last_nonce_ + 1;
    }
    last_nonce_ = value;
    return render_u64(value, nonce_);
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::finish(net::HttpMethod method,
                                                                         std::string_view endpoint,
                                                                         std::string_view post_data,
                                                                         UnixNanos now) {
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NotSigned};
    }
    const auto nonce = next_nonce(now);
    if (nonce.empty()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }

    Appender msg{sign_msg_};
    msg << post_data << nonce << endpoint;
    if (!msg.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    sign_msg_len_ = msg.view().size();

    const auto signature = authent(signer_->secret().view(), msg.view(), authent_);
    if (signature.empty()) {
        // Either the secret is not valid base64 or OpenSSL failed. Neither is
        // something to retry, and an empty Authent is never sent.
        return std::unexpected{BuildError::NotSigned};
    }
    authent_len_ = signature.size();

    Appender target{target_};
    target << ep::k_route_prefix << endpoint;
    if (method == net::HttpMethod::Get && !post_data.empty()) {
        target << "?" << post_data;
    }
    if (!target.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }

    net::RestRequest request{};
    request.method = method;
    request.target = target.view();
    request.body = method == net::HttpMethod::Post ? post_data : std::string_view{};
    request.headers = {
        {k_hdr_key, signer_->api_key()}, {k_hdr_nonce, nonce}, {k_hdr_authent, signature}};
    request.content_type = k_form_content_type;
    return request;
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::signed_post(
    std::string_view endpoint, std::string_view post_data, UnixNanos now) {
    return finish(net::HttpMethod::Post, endpoint, post_data, now);
}

std::expected<net::RestRequest, BuildError>
SignedRequestBuilder::signed_get(std::string_view endpoint, std::string_view query, UnixNanos now) {
    return finish(net::HttpMethod::Get, endpoint, query, now);
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
    const auto qty = order_qty(cmd, base);
    if (!qty.has_value()) {
        return std::unexpected{BuildError::BadQuantity};
    }

    Appender params{params_};
    params << "orderType=" << order_type_text(cmd) << "&symbol=" << base.exchange_symbol.view()
           << "&side=" << (cmd.side == Side::Buy ? "buy" : "sell") << "&size=" << *qty;
    if (cmd.order_type != OrderType::Market) {
        // Kraken refuses "mkt" outright when a limitPrice rides along, so the
        // field is absent rather than zero on a market order.
        const auto price = order_price(cmd, base);
        if (!price.has_value()) {
            return std::unexpected{BuildError::BadPrice};
        }
        params << "&limitPrice=" << *price;
    }
    if (has_flag(cmd.flags, OrderFlags::ReduceOnly)) {
        params << "&reduceOnly=true";
    }
    if (!cmd.exchange_client_order_id.empty()) {
        // A 26-character ULID against a 100-character field: no encoding is
        // needed because Crockford base32 is unreserved throughout.
        params << "&cliOrdId=" << cmd.exchange_client_order_id.view();
    }
    if (!params.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return signed_post(ep::k_send_order, params.view(), now);
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::cancel_order(
    const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
    if (const auto refusal = market_refusal(cmd.instrument.market()); refusal.has_value()) {
        return std::unexpected{*refusal};
    }
    if (table.find(cmd.instrument) == nullptr) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    Appender params{params_};
    // The venue's own id is preferred: a cliOrdId is unique only among orders
    // still working, so cancelling by it races an id the OMS has reused.
    if (!cmd.target_exchange_order_id.empty()) {
        params << "order_id=" << cmd.target_exchange_order_id.view();
    } else if (!cmd.exchange_client_order_id.empty()) {
        params << "cliOrdId=" << cmd.exchange_client_order_id.view();
    } else {
        return std::unexpected{BuildError::NoOrderTarget};
    }
    if (!params.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return signed_post(ep::k_cancel_order, params.view(), now);
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
    const auto qty = order_qty(cmd, base);
    const auto price = order_price(cmd, base);
    if (!qty.has_value()) {
        return std::unexpected{BuildError::BadQuantity};
    }
    if (!price.has_value()) {
        // editorder has no market form: an amend is always to a resting price.
        return std::unexpected{BuildError::BadPrice};
    }

    Appender params{params_};
    // `orderId`, not `order_id`. cancelorder spells the same field the other
    // way, and so do the two response objects.
    if (!cmd.target_exchange_order_id.empty()) {
        params << "orderId=" << cmd.target_exchange_order_id.view();
    } else if (!cmd.exchange_client_order_id.empty()) {
        params << "cliOrdId=" << cmd.exchange_client_order_id.view();
    } else {
        return std::unexpected{BuildError::NoOrderTarget};
    }
    params << "&size=" << *qty << "&limitPrice=" << *price;
    if (!params.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return signed_post(ep::k_edit_order, params.view(), now);
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::open_orders(UnixNanos now) {
    // No parameters at all: the signed message is nonce + path, and the empty
    // postData still hashes.
    return signed_get(ep::k_open_orders, {}, now);
}

std::expected<net::RestRequest, BuildError>
SignedRequestBuilder::fills(std::string_view last_fill_time, UnixNanos now) {
    if (last_fill_time.empty()) {
        return signed_get(ep::k_fills, {}, now);
    }
    Appender params{params_};
    // Echoed verbatim: the cursor is the instant the venue itself printed, and
    // its fractional width varies frame to frame. ':' is a legal query
    // character, which is why nothing percent-encodes it here.
    params << "lastFillTime=" << last_fill_time;
    if (!params.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return signed_get(ep::k_fills, params.view(), now);
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::positions(UnixNanos now) {
    return signed_get(ep::k_open_positions, {}, now);
}

} // namespace raikiri::venues::kraken
