#include <raikiri/venues/deribit/rest.hpp>

#include <charconv>
#include <cstring>

namespace raikiri::venues::deribit {

namespace {

constexpr std::string_view k_host_live = "www.deribit.com";
constexpr std::string_view k_host_test = "test.deribit.com";

constexpr std::string_view k_prefix = "/api/v2/";

constexpr std::string_view k_hdr_authorization = "Authorization";

// A rendered FixedE9 never needs more than sign + 19 digits + point.
constexpr std::size_t k_number_bytes = 32;

// Drops a part that would overflow rather than truncating mid-token, so an
// over-long currency yields a request Deribit refuses with Invalid params
// instead of one that names a different market.
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

// Reports overflow instead of truncating: a query cut mid-number names a size
// nobody asked for, and the signature would authenticate it.
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

    // Rendered from int64 fixed-point; no floating-point instruction is
    // reachable from an order price on this path.
    Appender& operator<<(FixedE9 value) noexcept {
        if (value.is_undef()) {
            overflow_ = true;
            return *this;
        }
        std::array<char, k_number_bytes> num{};
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

[[nodiscard]] std::string_view tif_text(TimeInForce tif) noexcept {
    switch (tif) {
    case TimeInForce::Gtc:
        return "good_til_cancelled";
    case TimeInForce::Ioc:
        return "immediate_or_cancel";
    case TimeInForce::Fok:
        return "fill_or_kill";
    }
    return "good_til_cancelled";
}

// The venue-corrected wall clock in milliseconds. Deribit rejects a signature
// whose timestamp is more than 60 seconds old, so this is the one place the
// clock offset the adapter measured actually has to be applied.
[[nodiscard]] std::uint64_t to_ms(UnixNanos now) noexcept {
    return now.ns / 1'000'000ULL;
}

// Exact conversion of an order quantity into the venue's `amount`. QtyLots
// counts steps of lot_size, and lot_size is min_trade_amount, so the product
// IS what Deribit wants — in USD for a reversed future and in coin for an
// option, decided by the instrument rather than by a branch here.
[[nodiscard]] std::optional<FixedE9> amount_of(const InstrumentBase& base, QtyLots qty) noexcept {
    if (qty.is_undef() || qty.raw <= 0) {
        return std::nullopt;
    }
    const auto amount = base.lots_to_qty(qty);
    if (amount.is_undef() || amount.raw <= 0) {
        return std::nullopt;
    }
    return amount;
}

[[nodiscard]] std::optional<FixedE9> price_of(const InstrumentBase& base,
                                              PriceTicks price) noexcept {
    if (price.is_undef()) {
        return std::nullopt;
    }
    const auto value = base.ticks_to_price(price);
    if (value.is_undef() || value.raw <= 0) {
        return std::nullopt;
    }
    return value;
}

} // namespace

std::string_view rest_host(bool testnet) noexcept {
    return testnet ? k_host_test : k_host_live;
}

std::string_view kind_of(MarketType market) noexcept {
    switch (market) {
    case MarketType::Perp:
    case MarketType::Future:
        return "future";
    case MarketType::Option:
        return "option";
    case MarketType::Spot:
        return "spot";
    }
    return "future";
}

std::string_view currency_of(std::string_view instrument_name) noexcept {
    const auto dash = instrument_name.find('-');
    return dash == std::string_view::npos ? std::string_view{} : instrument_name.substr(0, dash);
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
    case BuildError::UnknownCurrency:
        return "UnknownCurrency";
    case BuildError::SpotNotModelled:
        return "SpotNotModelled";
    case BuildError::NothingToAmend:
        return "NothingToAmend";
    }
    return "Unknown";
}

// Deribit's coin-margined complex is the perpetual, the dated future and the
// option series, and private/buy takes the same shape for all three. Spot is
// the one refusal, and it is named rather than implied.
std::optional<BuildError> market_refusal(MarketType market) noexcept {
    switch (market) {
    case MarketType::Perp:
    case MarketType::Future:
    case MarketType::Option:
        return std::nullopt;
    case MarketType::Spot:
        // Deribit routes spot to Coinbase Exchange and settles it in a
        // different currency model; map_instrument refuses every spot row, so
        // no spot instrument is ever in the table to order against.
        return BuildError::SpotNotModelled;
    }
    return BuildError::SpotNotModelled;
}

net::RestRequest RequestBuilder::server_time() noexcept {
    return {.method = net::HttpMethod::Get, .target = "/api/v2/public/get_time", .body = {}};
}

net::RestRequest RequestBuilder::instruments(std::string_view currency,
                                             std::string_view kind) noexcept {
    TargetWriter writer{target_};
    writer.put("/api/v2/public/get_instruments?currency=");
    writer.put(currency);
    writer.put("&kind=");
    writer.put(kind);
    return writer.get();
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::sign(std::string_view target,
                                                                       UnixNanos now) {
    if (target.empty()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NotSigned};
    }
    const auto timestamp = to_ms(now);
    const auto nonce = sign::nonce(++nonce_counter_, nonce_);

    // The `data` term is the request itself: METHOD \n uri \n body \n. Every
    // private call here is a GET whose body is empty, and the trailing newline
    // stays regardless — Deribit's own worked example emits it.
    std::array<char, 896> data{};
    const auto request_data = sign::request_data(net::HttpMethod::Get, target, {}, data);
    const auto message = sign::message(timestamp, nonce, request_data, sign_msg_);
    if (request_data.empty() || message.empty()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    sign_msg_len_ = message.size();

    const auto signature = signer_->sign(message, net::Signer::Encoding::Hex, signature_);
    const auto header = sign::auth_header(*signer_, timestamp, nonce, signature, header_);
    if (header.empty()) {
        return std::unexpected{BuildError::NotSigned};
    }

    net::RestRequest request{};
    request.method = net::HttpMethod::Get;
    request.target = target;
    request.headers = {{k_hdr_authorization, header}};
    return request;
}

std::string_view SignedRequestBuilder::last_sign_message() const noexcept {
    return {sign_msg_.data(), sign_msg_len_};
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
    const auto amount = amount_of(base, cmd.qty);
    if (!amount.has_value()) {
        return std::unexpected{BuildError::BadQuantity};
    }
    if (cmd.exchange_client_order_id.empty()) {
        return std::unexpected{BuildError::NoOrderTarget};
    }

    const bool is_market = cmd.order_type == OrderType::Market;
    const bool post_only = has_flag(cmd.flags, OrderFlags::PostOnly) && !is_market;

    Appender target{target_};
    target << k_prefix << (cmd.side == Side::Sell ? "private/sell" : "private/buy")
           << "?instrument_name=" << base.exchange_symbol.view() << "&amount=" << *amount
           << "&label=" << cmd.exchange_client_order_id.view()
           << "&type=" << (is_market ? "market" : "limit");
    if (!is_market) {
        const auto price = price_of(base, cmd.price);
        if (!price.has_value()) {
            return std::unexpected{BuildError::BadPrice};
        }
        target << "&price=" << *price << "&time_in_force=" << tif_text(cmd.tif);
    }
    // post_only DEFAULTS TO TRUE on Deribit, alone among the venues here, so
    // it is stated on every order — a market order that inherited the default
    // would be refused outright. reject_post_only rides with it because
    // Deribit's own post-only behaviour is to REPRICE an order that would
    // cross, and an order resting at a price the strategy never chose is worse
    // than one that never rested.
    target << "&post_only=" << (post_only ? "true" : "false");
    if (post_only) {
        target << "&reject_post_only=true";
    }
    if (has_flag(cmd.flags, OrderFlags::ReduceOnly)) {
        target << "&reduce_only=true";
    }
    if (!target.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(target.view(), now);
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::cancel_order(
    const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
    if (const auto refusal = market_refusal(cmd.instrument.market()); refusal.has_value()) {
        return std::unexpected{*refusal};
    }
    Appender target{target_};
    if (!cmd.target_exchange_order_id.empty()) {
        target << k_prefix << "private/cancel?order_id="
               << cmd.target_exchange_order_id.view();
    } else if (!cmd.exchange_client_order_id.empty()) {
        const auto symbol = table.exchange_symbol(cmd.instrument);
        if (symbol.empty()) {
            return std::unexpected{BuildError::UnknownInstrument};
        }
        const auto currency = currency_of(symbol);
        if (currency.empty()) {
            return std::unexpected{BuildError::UnknownCurrency};
        }
        // Scoped to the currency on purpose: an unscoped cancel_by_label
        // sweeps every currency, and a label collision across coins would
        // cancel an order this command never named.
        target << k_prefix << "private/cancel_by_label?label="
               << cmd.exchange_client_order_id.view() << "&currency=" << currency;
    } else {
        return std::unexpected{BuildError::NoOrderTarget};
    }
    if (!target.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(target.view(), now);
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

    Appender target{target_};
    if (!cmd.target_exchange_order_id.empty()) {
        target << k_prefix << "private/edit?order_id=" << cmd.target_exchange_order_id.view();
    } else if (!cmd.exchange_client_order_id.empty()) {
        // edit_by_label works only while EXACTLY ONE open order carries the
        // label. A ULID is minted per order, so that holds by construction —
        // and the instrument is required beside it.
        target << k_prefix << "private/edit_by_label?label="
               << cmd.exchange_client_order_id.view()
               << "&instrument_name=" << base.exchange_symbol.view();
    } else {
        return std::unexpected{BuildError::NoOrderTarget};
    }

    bool changed = false;
    if (!cmd.qty.is_undef()) {
        const auto amount = amount_of(base, cmd.qty);
        if (!amount.has_value()) {
            return std::unexpected{BuildError::BadQuantity};
        }
        target << "&amount=" << *amount;
        changed = true;
    }
    if (!cmd.price.is_undef()) {
        const auto price = price_of(base, cmd.price);
        if (!price.has_value()) {
            return std::unexpected{BuildError::BadPrice};
        }
        target << "&price=" << *price;
        changed = true;
    }
    if (!changed) {
        return std::unexpected{BuildError::NothingToAmend};
    }
    if (!target.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(target.view(), now);
}

std::expected<net::RestRequest, BuildError>
SignedRequestBuilder::open_orders(MarketType market, std::string_view currency, UnixNanos now) {
    if (const auto refusal = market_refusal(market); refusal.has_value()) {
        return std::unexpected{*refusal};
    }
    if (currency.empty()) {
        return std::unexpected{BuildError::UnknownCurrency};
    }
    Appender target{target_};
    // "future" covers the perpetual AND the dated series; the caller keeps
    // only the rows whose instrument resolves to the market it asked for.
    target << k_prefix << "private/get_open_orders_by_currency?currency=" << currency
           << "&kind=" << kind_of(market);
    if (!target.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(target.view(), now);
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::order_state_by_label(
    const ExchangeClientOrderId& id, std::string_view currency, UnixNanos now) {
    if (id.empty()) {
        return std::unexpected{BuildError::NoOrderTarget};
    }
    if (currency.empty()) {
        return std::unexpected{BuildError::UnknownCurrency};
    }
    Appender target{target_};
    target << k_prefix << "private/get_order_state_by_label?currency=" << currency
           << "&label=" << id.view();
    if (!target.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(target.view(), now);
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::user_trades(
    MarketType market, std::string_view currency, UnixNanos since, UnixNanos now) {
    if (const auto refusal = market_refusal(market); refusal.has_value()) {
        return std::unexpected{*refusal};
    }
    if (currency.empty()) {
        return std::unexpected{BuildError::UnknownCurrency};
    }
    Appender target{target_};
    target << k_prefix << "private/get_user_trades_by_currency?currency=" << currency
           << "&kind=" << kind_of(market)
           << "&count=" << static_cast<std::uint64_t>(k_max_trade_page);
    if (since.ns != 0) {
        // Ascending from the watermark, so a page that fills leaves the OLDEST
        // unseen fills behind rather than the newest — the caller can advance
        // and ask again.
        target << "&start_timestamp=" << to_ms(since) << "&sorting=asc";
    }
    if (!target.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(target.view(), now);
}

std::expected<net::RestRequest, BuildError>
SignedRequestBuilder::positions(MarketType market, std::string_view currency, UnixNanos now) {
    if (const auto refusal = market_refusal(market); refusal.has_value()) {
        return std::unexpected{*refusal};
    }
    if (currency.empty()) {
        return std::unexpected{BuildError::UnknownCurrency};
    }
    Appender target{target_};
    target << k_prefix << "private/get_positions?currency=" << currency
           << "&kind=" << kind_of(market);
    if (!target.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(target.view(), now);
}

} // namespace raikiri::venues::deribit
