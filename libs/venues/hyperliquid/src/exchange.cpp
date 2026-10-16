#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/hyperliquid/exchange.hpp>
#include <raikiri/venues/hyperliquid/map.hpp>

#include <charconv>
#include <cstring>

namespace raikiri::venues::hyperliquid {

namespace {

constexpr std::string_view k_exchange = "/exchange";

// msgpack keys keep insertion order and 32..255-byte strings use str8;
// the json and packed forms share one call sequence
class ActionWriter {
public:
    ActionWriter(std::span<std::uint8_t> pack, std::span<char> json) noexcept : pack_{pack}, json_{json} {}

    ActionWriter& map(std::size_t entries) noexcept {
        if (entries >= 16) {
            overflow_ = true; // fixmap only; no action here comes close
            return *this;
        }
        put_byte(static_cast<std::uint8_t>(0x80U | entries));
        separator();
        put_char('{');
        depth_comma_ = false;
        return *this;
    }

    ActionWriter& end_map() noexcept {
        put_char('}');
        depth_comma_ = true;
        return *this;
    }

    ActionWriter& array(std::size_t entries) noexcept {
        if (entries >= 16) {
            overflow_ = true;
            return *this;
        }
        put_byte(static_cast<std::uint8_t>(0x90U | entries));
        separator();
        put_char('[');
        depth_comma_ = false;
        return *this;
    }

    ActionWriter& end_array() noexcept {
        put_char(']');
        depth_comma_ = true;
        return *this;
    }

    ActionWriter& key(std::string_view name) noexcept {
        pack_str(name);
        separator();
        put_char('"');
        put_text(name);
        put_char('"');
        put_char(':');
        pending_value_ = true;
        return *this;
    }

    ActionWriter& str(std::string_view value) noexcept {
        pack_str(value);
        separator();
        put_char('"');
        put_text(value);
        put_char('"');
        return *this;
    }

    ActionWriter& boolean(bool value) noexcept {
        put_byte(value ? 0xC3U : 0xC2U);
        separator();
        put_text(value ? "true" : "false");
        return *this;
    }

    ActionWriter& integer(std::uint64_t value) noexcept {
        // minimal width, since a wider encoding hashes differently
        if (value < 0x80U) {
            put_byte(static_cast<std::uint8_t>(value));
        } else if (value <= 0xFFU) {
            put_byte(0xCCU);
            put_be(value, 1);
        } else if (value <= 0xFFFFU) {
            put_byte(0xCDU);
            put_be(value, 2);
        } else if (value <= 0xFFFF'FFFFULL) {
            put_byte(0xCEU);
            put_be(value, 4);
        } else {
            put_byte(0xCFU);
            put_be(value, 8);
        }
        separator();
        std::array<char, 24> digits{};
        const auto [stop, ec] = std::to_chars(digits.data(), digits.data() + digits.size(), value);
        if (ec != std::errc{}) {
            overflow_ = true;
            return *this;
        }
        put_text(std::string_view{digits.data(), static_cast<std::size_t>(stop - digits.data())});
        return *this;
    }

    [[nodiscard]] bool ok() const noexcept { return !overflow_; }

    [[nodiscard]] std::span<const std::uint8_t> packed() const noexcept {
        return overflow_ ? std::span<const std::uint8_t>{} : pack_.first(pack_len_);
    }

    [[nodiscard]] std::string_view json() const noexcept {
        return overflow_ ? std::string_view{} : std::string_view{json_.data(), json_len_};
    }

private:
    void put_byte(std::uint8_t byte) noexcept {
        if (overflow_ || pack_len_ == pack_.size()) {
            overflow_ = true;
            return;
        }
        pack_[pack_len_++] = byte;
    }

    void put_be(std::uint64_t value, std::size_t width) noexcept {
        for (std::size_t i = 0; i < width; ++i) {
            put_byte(static_cast<std::uint8_t>(value >> (8U * (width - 1 - i))));
        }
    }

    void pack_str(std::string_view text) noexcept {
        if (text.size() < 32) {
            put_byte(static_cast<std::uint8_t>(0xA0U | text.size()));
        } else if (text.size() < 256) {
            put_byte(0xD9U); // str8: the cloid is 34 bytes
            put_byte(static_cast<std::uint8_t>(text.size()));
        } else {
            overflow_ = true;
            return;
        }
        for (const char c : text) {
            put_byte(static_cast<std::uint8_t>(c));
        }
    }

    void put_char(char c) noexcept {
        if (overflow_ || json_len_ == json_.size()) {
            overflow_ = true;
            return;
        }
        json_[json_len_++] = c;
    }

    void put_text(std::string_view text) noexcept {
        // values are literals, hex ids and digits, so nothing needs escaping
        for (const char c : text) {
            if (c == '"' || c == '\\') {
                overflow_ = true;
                return;
            }
            put_char(c);
        }
    }

    void separator() noexcept {
        if (pending_value_) {
            pending_value_ = false;
            return;
        }
        if (depth_comma_) {
            put_char(',');
        }
        depth_comma_ = true;
    }

    std::span<std::uint8_t> pack_;
    std::span<char> json_;
    std::size_t pack_len_{0};
    std::size_t json_len_{0};
    bool overflow_{false};
    bool depth_comma_{false};
    bool pending_value_{false};
};

[[nodiscard]] std::string_view render(FixedE9 value, std::span<char> out) noexcept {
    return format_e9(value, out.data(), out.size());
}

[[nodiscard]] std::string_view tif_of(const OrderCmd& cmd) noexcept {
    if (has_flag(cmd.flags, OrderFlags::PostOnly)) {
        return "Alo";
    }
    return cmd.tif == TimeInForce::Ioc || cmd.order_type == OrderType::Market ? "Ioc" : "Gtc";
}

[[nodiscard]] std::optional<BuildError> refuse_terms(const OrderCmd& cmd) noexcept {
    switch (cmd.tif) {
    case TimeInForce::Gtc:
    case TimeInForce::Ioc:
        break;
    case TimeInForce::Fok:
        return BuildError::FokUnsupported;
    }
    if (has_flag(cmd.flags, OrderFlags::PostOnly) &&
        (cmd.tif == TimeInForce::Ioc || cmd.order_type == OrderType::Market)) {
        return BuildError::PostOnlyConflictsWithTif;
    }
    if (cmd.order_type == OrderType::Market && (cmd.price.is_undef() || cmd.price.raw <= 0)) {
        return BuildError::MarketOrderNeedsLimit;
    }
    return std::nullopt;
}

// checked before the table lookup, so an unserved market is not reported as UnknownInstrument
[[nodiscard]] std::optional<BuildError> market_refusal(MarketType market) noexcept {
    switch (market) {
    case MarketType::Spot:
    case MarketType::Perp:
        return std::nullopt;
    case MarketType::Future:
        return BuildError::DatedFuturesUnsupported;
    case MarketType::Option:
        return BuildError::OptionsUnsupported;
    }
    return BuildError::OptionsUnsupported;
}

} // namespace

std::string_view to_string(BuildError err) noexcept {
    switch (err) {
    case BuildError::UnknownInstrument:
        return "unknown instrument";
    case BuildError::UnknownAsset:
        return "no venue asset index for instrument";
    case BuildError::BadQuantity:
        return "quantity not expressible";
    case BuildError::BadPrice:
        return "price not expressible";
    case BuildError::TooManySignificantDigits:
        return "price exceeds five significant figures";
    case BuildError::BufferTooSmall:
        return "action exceeded the scratch buffer";
    case BuildError::NotSigned:
        return "no signing key loaded";
    case BuildError::NoOrderTarget:
        return "neither a venue order id nor a client order id";
    case BuildError::MarketOrderNeedsLimit:
        return "hyperliquid has no market order; supply a limit price";
    case BuildError::PostOnlyConflictsWithTif:
        return "post-only is a time-in-force here and cannot join another";
    case BuildError::FokUnsupported:
        return "hyperliquid offers Alo, Ioc and Gtc only";
    case BuildError::OptionsUnsupported:
        return "hyperliquid options order entry";
    case BuildError::DatedFuturesUnsupported:
        return "hyperliquid dated futures order entry";
    }
    return "unknown build error";
}

std::uint64_t NonceSource::next(UnixNanos now) noexcept {
    // strictly increasing even if the clock steps back
    const auto ms = now.ns / 1'000'000ULL;
    last_ = ms > last_ ? ms : last_ + 1;
    return last_;
}

bool price_digits_ok(std::string_view text) noexcept {
    const auto dot = text.find('.');
    if (dot == std::string_view::npos) {
        return true; // integer prices are always accepted
    }
    std::size_t significant = 0;
    bool started = false;
    for (const char c : text) {
        if (c == '.' || c == '-') {
            continue;
        }
        if (!started && c == '0') {
            continue; // leading zeroes are not significant, on either side
        }
        started = true;
        ++significant;
    }
    return significant <= 5;
}

std::string_view five_figures(std::string_view text, bool up, std::span<char> out) noexcept {
    // a leading zero gives a carry out of the first digit somewhere to land
    if (text.size() + 1 > out.size()) {
        return {};
    }
    out[0] = '0';
    std::memcpy(out.data() + 1, text.data(), text.size());
    const std::size_t size = text.size() + 1;
    std::size_t significant = 0;
    std::size_t last_kept = 0;
    bool started = false;
    bool dropped = false;
    for (std::size_t i = 1; i < size; ++i) {
        if (out[i] == '.' || (!started && out[i] == '0')) {
            continue;
        }
        started = true;
        if (significant < 5) {
            ++significant;
            last_kept = i;
            continue;
        }
        dropped = dropped || out[i] != '0';
        out[i] = '0';
    }
    if (dropped && up) {
        for (std::size_t i = last_kept;; --i) {
            if (out[i] == '.') {
                continue;
            }
            if (out[i] != '9') {
                ++out[i];
                break;
            }
            out[i] = '0';
        }
    }
    std::string_view rounded{out.data(), size};
    if (rounded.find('.') != std::string_view::npos) {
        while (rounded.back() == '0') {
            rounded.remove_suffix(1);
        }
        if (rounded.back() == '.') {
            rounded.remove_suffix(1);
        }
    }
    if (rounded.size() > 1 && rounded[0] == '0' && rounded[1] != '.') {
        rounded.remove_prefix(1);
    }
    return rounded;
}

std::expected<net::RestRequest, BuildError> ExchangeBuilder::create_order(const OrderCmd& cmd,
                                                                          const exec::InstrumentTable& table,
                                                                          std::int64_t asset,
                                                                          std::uint64_t nonce) {
    if (const auto refusal = market_refusal(cmd.instrument.market()); refusal.has_value()) {
        return std::unexpected{*refusal};
    }
    if (const auto refusal = refuse_terms(cmd); refusal.has_value()) {
        return std::unexpected{*refusal};
    }
    if (!wallet_->loaded()) {
        return std::unexpected{BuildError::NotSigned};
    }
    if (asset < 0) {
        return std::unexpected{BuildError::UnknownAsset};
    }
    const auto* instrument = table.find(cmd.instrument);
    if (instrument == nullptr) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    const auto& base = base_of(*instrument);

    std::array<char, 32> price_buf{};
    std::array<char, 32> qty_buf{};
    auto price = render(base.ticks_to_price(cmd.price), price_buf);
    const auto qty = render(base.lots_to_qty(cmd.qty), qty_buf);
    if (price.empty() || cmd.price.raw <= 0) {
        return std::unexpected{BuildError::BadPrice};
    }
    if (qty.empty() || cmd.qty.raw <= 0) {
        return std::unexpected{BuildError::BadQuantity};
    }
    std::array<char, 34> rounded_buf{};
    if (!price_digits_ok(price)) {
        // an ioc's limit only bounds its fill, so it moves to five figures on the passive side;
        // a resting price is the strategy's quote and is refused instead
        if (cmd.tif != TimeInForce::Ioc && cmd.order_type != OrderType::Market) {
            return std::unexpected{BuildError::TooManySignificantDigits};
        }
        price = five_figures(price, cmd.side == Side::Sell, rounded_buf);
        if (price.empty()) {
            return std::unexpected{BuildError::BadPrice};
        }
    }
    std::array<char, k_cloid_chars> cloid_buf{};
    const auto cloid = cloid_text(cmd.exchange_client_order_id, cloid_buf);

    ActionWriter action{packed_, action_json_};
    action.map(3);
    action.key("type").str("order");
    action.key("orders").array(1);
    action.map(cloid.empty() ? 6 : 7);
    action.key("a").integer(static_cast<std::uint64_t>(asset));
    action.key("b").boolean(cmd.side == Side::Buy);
    action.key("p").str(price);
    action.key("s").str(qty);
    action.key("r").boolean(has_flag(cmd.flags, OrderFlags::ReduceOnly));
    action.key("t").map(1);
    action.key("limit").map(1);
    action.key("tif").str(tif_of(cmd));
    action.end_map().end_map();
    if (!cloid.empty()) {
        action.key("c").str(cloid);
    }
    action.end_map();
    action.end_array();
    // always written; the reference sdk hashes it unconditionally. a priority rate is honoured on ioc orders only
    if (cmd.priority_e8 != 0 && tif_of(cmd) == "Ioc") {
        action.key("grouping").map(1);
        action.key("p").integer(cmd.priority_e8);
        action.end_map();
    } else {
        action.key("grouping").str("na");
    }
    action.end_map();
    if (!action.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign_and_wrap(action.packed(), action.json(), nonce);
}

std::expected<net::RestRequest, BuildError>
ExchangeBuilder::cancel_order(const OrderCmd& cmd, std::int64_t asset, std::uint64_t nonce) {
    if (const auto refusal = market_refusal(cmd.instrument.market()); refusal.has_value()) {
        return std::unexpected{*refusal};
    }
    if (!wallet_->loaded()) {
        return std::unexpected{BuildError::NotSigned};
    }
    if (asset < 0) {
        return std::unexpected{BuildError::UnknownAsset};
    }
    const auto oid = digits_to_oid(cmd.target_exchange_order_id.view());
    std::array<char, k_cloid_chars> cloid_buf{};
    const auto cloid = cloid_text(cmd.exchange_client_order_id, cloid_buf);
    if (oid == 0 && cloid.empty()) {
        return std::unexpected{BuildError::NoOrderTarget};
    }

    // fast: cancels jump the mempool queue; only trigger orders refuse it, and we place none
    ActionWriter action{packed_, action_json_};
    action.map(3);
    if (oid != 0) {
        action.key("type").str("cancel");
        action.key("cancels").array(1).map(2);
        action.key("a").integer(static_cast<std::uint64_t>(asset));
        action.key("o").integer(oid);
    } else {
        action.key("type").str("cancelByCloid");
        action.key("cancels").array(1).map(2);
        action.key("asset").integer(static_cast<std::uint64_t>(asset));
        action.key("cloid").str(cloid);
    }
    action.end_map().end_array();
    action.key("f").boolean(true);
    action.end_map();
    if (!action.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign_and_wrap(action.packed(), action.json(), nonce);
}

std::expected<net::RestRequest, BuildError> ExchangeBuilder::modify_order(const OrderCmd& cmd,
                                                                          const exec::InstrumentTable& table,
                                                                          std::int64_t asset,
                                                                          std::uint64_t nonce) {
    if (const auto refusal = market_refusal(cmd.instrument.market()); refusal.has_value()) {
        return std::unexpected{*refusal};
    }
    if (const auto refusal = refuse_terms(cmd); refusal.has_value()) {
        return std::unexpected{*refusal};
    }
    if (!wallet_->loaded()) {
        return std::unexpected{BuildError::NotSigned};
    }
    if (asset < 0) {
        return std::unexpected{BuildError::UnknownAsset};
    }
    const auto* instrument = table.find(cmd.instrument);
    if (instrument == nullptr) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    const auto& base = base_of(*instrument);
    const auto oid = digits_to_oid(cmd.target_exchange_order_id.view());
    std::array<char, k_cloid_chars> cloid_buf{};
    const auto cloid = cloid_text(cmd.exchange_client_order_id, cloid_buf);
    if (oid == 0 && cloid.empty()) {
        return std::unexpected{BuildError::NoOrderTarget};
    }

    std::array<char, 32> price_buf{};
    std::array<char, 32> qty_buf{};
    const auto price = render(base.ticks_to_price(cmd.price), price_buf);
    const auto qty = render(base.lots_to_qty(cmd.qty), qty_buf);
    if (price.empty() || cmd.price.raw <= 0) {
        return std::unexpected{BuildError::BadPrice};
    }
    if (qty.empty() || cmd.qty.raw <= 0) {
        return std::unexpected{BuildError::BadQuantity};
    }
    if (!price_digits_ok(price)) {
        return std::unexpected{BuildError::TooManySignificantDigits};
    }

    ActionWriter action{packed_, action_json_};
    action.map(3);
    action.key("type").str("modify");
    // an oid is a msgpack number, a cloid a string
    action.key("oid");
    if (oid != 0) {
        action.integer(oid);
    } else {
        action.str(cloid);
    }
    action.key("order").map(cloid.empty() ? 6 : 7);
    action.key("a").integer(static_cast<std::uint64_t>(asset));
    action.key("b").boolean(cmd.side == Side::Buy);
    action.key("p").str(price);
    action.key("s").str(qty);
    action.key("r").boolean(has_flag(cmd.flags, OrderFlags::ReduceOnly));
    action.key("t").map(1);
    action.key("limit").map(1);
    action.key("tif").str(tif_of(cmd));
    action.end_map().end_map();
    if (!cloid.empty()) {
        // the modify keeps the client id; it cancels and places atomically
        action.key("c").str(cloid);
    }
    action.end_map();
    action.end_map();
    if (!action.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign_and_wrap(action.packed(), action.json(), nonce);
}

std::uint64_t ExchangeBuilder::digits_to_oid(std::string_view text) noexcept {
    std::uint64_t value = 0;
    const auto [stop, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && stop == text.data() + text.size() ? value : 0;
}

std::expected<net::RestRequest, BuildError> ExchangeBuilder::sign_and_wrap(std::span<const std::uint8_t> packed,
                                                                           std::string_view action_json,
                                                                           std::uint64_t nonce) {
    // vault and expiry change the preimage and are not supported here
    const auto hash = action_hash(packed, nonce, {}, 0);
    Signature signature{};
    if (!wallet_->sign(agent_digest(hash, mainnet_), signature)) {
        return std::unexpected{BuildError::NotSigned};
    }

    std::size_t at = 0;
    const auto put = [&](std::string_view text) {
        if (at + text.size() > body_.size()) {
            at = body_.size() + 1; // sticky overflow, checked once below
            return;
        }
        text.copy(body_.data() + at, text.size());
        at += text.size();
    };
    std::array<char, 24> nonce_buf{};
    const auto [stop, ec] = std::to_chars(nonce_buf.data(), nonce_buf.data() + nonce_buf.size(), nonce);
    if (ec != std::errc{}) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    std::array<char, 4> v_buf{};
    const auto [v_stop, v_ec] = std::to_chars(v_buf.data(), v_buf.data() + v_buf.size(), signature.v);
    if (v_ec != std::errc{}) {
        return std::unexpected{BuildError::BufferTooSmall};
    }

    put(R"({"action":)");
    put(action_json);
    put(R"(,"nonce":)");
    put(std::string_view{nonce_buf.data(), static_cast<std::size_t>(stop - nonce_buf.data())});
    put(R"(,"signature":{"r":")");
    put(signature.r.view());
    put(R"(","s":")");
    put(signature.s.view());
    put(R"(","v":)");
    put(std::string_view{v_buf.data(), static_cast<std::size_t>(v_stop - v_buf.data())});
    put(R"(},"vaultAddress":null})");
    if (at > body_.size()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }

    return net::RestRequest{
        .method = net::HttpMethod::Post, .target = k_exchange, .body = std::string_view{body_.data(), at}};
}

} // namespace raikiri::venues::hyperliquid
