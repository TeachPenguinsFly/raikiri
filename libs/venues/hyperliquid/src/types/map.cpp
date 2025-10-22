#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/int128.hpp>
#include <raikiri/venues/hyperliquid/account.hpp>
#include <raikiri/venues/hyperliquid/book.hpp>
#include <raikiri/venues/hyperliquid/catalogue.hpp>
#include <raikiri/venues/hyperliquid/map.hpp>
#include <raikiri/venues/hyperliquid/market_data.hpp>
#include <raikiri/venues/hyperliquid/orders.hpp>
#include <raikiri/venues/hyperliquid/positions.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <string_view>

namespace raikiri::venues::hyperliquid {

namespace {

// A PERP price is capped at 6 - szDecimals decimal places; spot uses 8, which
// is why this is named for its market rather than being a bare 6.
constexpr std::int64_t k_perp_price_digits = 6;

// The same rule on the spot book, two places finer. A pair loaded with the perp
// exponent quotes every price a hundred times off its own grid.
constexpr std::int64_t k_spot_price_digits = 8;

// Exact conversion against the instrument's own step. A value that does not
// divide evenly means the venue changed an instrument's precision, so the
// message is dropped rather than rounded — rounding is how a mark price ends
// up one tick away from the one the venue published.
std::optional<std::int64_t> exact_steps(std::string_view text, FixedE9 step) {
    if (text.empty() || step.raw <= 0) {
        return std::nullopt;
    }
    const auto value = parse_decimal_e9(text);
    if (!value.has_value() || value->raw % step.raw != 0) {
        return std::nullopt;
    }
    return value->raw / step.raw;
}

std::optional<PriceTicks> to_ticks(std::string_view text, const InstrumentBase& base) {
    const auto steps = exact_steps(text, base.tick_size);
    return steps.has_value() ? std::optional{PriceTicks::from_raw(*steps)} : std::nullopt;
}

std::optional<QtyLots> to_lots(std::string_view text, const InstrumentBase& base) {
    const auto steps = exact_steps(text, base.lot_size);
    return steps.has_value() ? std::optional{QtyLots::from_raw(*steps)} : std::nullopt;
}

// Venue-scale, not instrument-scaled: rates stay e9 rather than becoming ticks.
std::optional<FixedE9> to_e9(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? std::optional{*value} : std::nullopt;
}

// A 24-hour aggregate is an AMOUNT: the venue writes dayNtlVlm to ten
// fractional digits, which the exact reader above refuses outright. Truncating
// below 1e-9 errs downward rather than dropping the whole figure.
std::optional<FixedE9> to_amount_e9(std::string_view text) {
    return text.empty() ? std::nullopt : parse_amount_e9(text);
}

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * 1'000'000ULL};
}

std::optional<FixedE9> step_from_decimals(std::int64_t decimals) noexcept {
    // FixedE9 holds nine fractional digits; finer has no representation.
    if (decimals < 0 || decimals > 9) {
        return std::nullopt;
    }
    std::int64_t raw = k_e9;
    for (std::int64_t i = 0; i < decimals; ++i) {
        raw /= 10;
    }
    return FixedE9::from_raw(raw);
}

std::optional<Bbo>
map_bbo(const WireBbo& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.coin);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto bid = to_ticks(wire.bid_price, base);
    const auto ask = to_ticks(wire.ask_price, base);
    const auto bid_qty = to_lots(wire.bid_qty, base);
    const auto ask_qty = to_lots(wire.ask_qty, base);
    if (!bid.has_value() || !ask.has_value() || !bid_qty.has_value() || !ask_qty.has_value()) {
        return std::nullopt; // includes a side the venue sent as JSON null
    }

    Bbo out{};
    out.instrument = base.id;
    out.bid = *bid;
    out.ask = *ask;
    out.bid_qty = *bid_qty;
    out.ask_qty = *ask_qty;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.coin);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto price = to_ticks(wire.price, base);
    const auto qty = to_lots(wire.qty, base);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }

    TradeTick out{};
    out.instrument = base.id;
    out.price = *price;
    out.qty = *qty;
    out.aggressor = wire.aggressor_bought ? Side::Buy : Side::Sell;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate>
map_funding(const WireAssetCtx& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.coin);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto rate = to_e9(wire.funding);
    if (!rate.has_value()) {
        return std::nullopt; // no funding quoted: not the same as zero
    }

    FundingUpdate out{};
    const auto& base = base_of(*instrument);
    out.instrument = base.id;
    out.rate = *rate;
    if (const auto mark = to_ticks(wire.mark_price, base); mark.has_value()) {
        out.mark_price = *mark;
    }
    out.interval_s = k_funding_interval_s;
    // No timestamp on this payload at all, so ingest is the only honest one.
    out.ts_event = ts_init;
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats>
map_ticker_stats(const WireAssetCtx& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    // ABSENT IS NOT ZERO. The REST rows this struct also carries name no coin
    // at all, and a context stating neither figure says nothing about volume.
    if (wire.day_base_volume.empty() && wire.day_notional_volume.empty()) {
        return std::nullopt;
    }
    const auto* instrument = table.find(wire.coin);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base_of(*instrument).id;
    out.volume_24h_base = to_amount_e9(wire.day_base_volume).value_or(FixedE9::from_raw(0));
    out.volume_24h_quote = to_amount_e9(wire.day_notional_volume).value_or(FixedE9::from_raw(0));
    // No timestamp on either transport, so ingest is the only honest one.
    out.ts_event = ts_init;
    out.ts_init = ts_init;
    return out;
}

std::optional<Instrument> map_instrument(const WireAsset& wire, MarketType market) {
    if (wire.delisted) {
        return std::nullopt;
    }
    if (market != MarketType::Perp) {
        return std::nullopt;
    }

    // DERIVED, NOT VENUE-STATED.
    const auto lot = step_from_decimals(wire.sz_decimals);
    const auto tick = step_from_decimals(k_perp_price_digits - wire.sz_decimals);
    if (!lot.has_value() || !tick.has_value()) {
        return std::nullopt;
    }
    // Bare ticker: "BTC", not "BTC-USDT-SWAP". Every perp is USDC-margined.
    const auto symbol = SymbolText::from(wire.name);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // Provisional: InstrumentTable::add assigns the SymbolId that indexes it.
    base.id = InstrumentId::make(VenueId::Hyperliquid, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *symbol;
    base.tick_size = *tick;
    base.lot_size = *lot;
    // Size is the coin itself, so there is no contract multiplier to load.
    base.contract_value = *parse_decimal_e9("1");
    base.kind = ContractKind::Linear;
    return PerpInstrument{base, k_funding_interval_s};
}

std::optional<BookSnapshot>
map_book(const WireBook& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.coin);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);

    BookSnapshot out{};
    out.instrument = base.id;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;

    const auto fill = [&](std::span<const WireLevel> src,
                          std::array<BookLevel, k_book_levels>& dst) {
        std::uint32_t n = 0;
        for (const auto& level : src) {
            if (n == k_book_levels) {
                break;
            }
            const auto price = to_ticks(level.price, base);
            const auto qty = to_lots(level.qty, base);
            // A level that does not divide by the instrument's step truncates
            // the book here rather than being rounded into a price the venue
            // never quoted.
            if (!price.has_value() || !qty.has_value()) {
                break;
            }
            dst[n] = {.price = *price, .qty = *qty};
            ++n;
        }
        return n;
    };
    out.bid_count = fill(wire.bids, out.bids);
    out.ask_count = fill(wire.asks, out.asks);
    if (out.bid_count == 0 && out.ask_count == 0) {
        return std::nullopt;
    }
    return out;
}

// ---------------------------------------------------------- account ----

namespace {

// `szi` is the one account field this venue serialises from a FLOAT: a live
// response carries "-4216984.4000000004" for a position whose lot is 0.1.
// Truncating below 1e-9 drops dust no step this domain can express could have
// carried, and the exact division by the lot below still refuses a size
// genuinely off the grid. For `szi` and nothing else.
std::optional<QtyLots> to_float_lots(std::string_view text, const InstrumentBase& base) {
    if (text.empty() || base.lot_size.raw <= 0) {
        return std::nullopt;
    }
    const auto value = parse_rate_e9(text);
    if (!value.has_value() || value->raw % base.lot_size.raw != 0) {
        return std::nullopt;
    }
    return QtyLots::from_raw(value->raw / base.lot_size.raw);
}

// A fee is money, but this venue pads a rebate past 1e-9 of a token and an
// exact reader would drop the whole fill over the eleventh digit. NO sign flip:
// a charge is already positive and a rebate negative, the ledger's own
// convention, and okx's inversion here would turn every rebate into a cost.
FixedE9 fee_e9(std::string_view text) noexcept {
    if (text.empty()) {
        return FixedE9::from_raw(0);
    }
    const auto value = parse_rate_e9(text);
    return value.has_value() ? *value : FixedE9::from_raw(0);
}

// The alphabet UlidGen emits. Lowercase is NOT accepted: the generator never
// produces it, so a lowercase id is someone else's wearing our shape.
constexpr std::string_view k_crockford = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

constexpr std::size_t k_ulid_chars = 26;

[[nodiscard]] int crockford_value(char c) noexcept {
    const auto at = k_crockford.find(c);
    return at == std::string_view::npos ? -1 : static_cast<int>(at);
}

[[nodiscard]] int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// `oid` and `tid` arrive as JSON numbers; the domain carries both as text.
template <typename Id> [[nodiscard]] std::optional<Id> id_from(std::uint64_t value) noexcept {
    std::array<char, 24> digits{};
    const auto [stop, ec] = std::to_chars(digits.data(), digits.data() + digits.size(), value);
    if (ec != std::errc{}) {
        return std::nullopt;
    }
    const auto text =
        std::string_view{digits.data(), static_cast<std::size_t>(stop - digits.data())};
    const auto id = Id::from(text);
    return id.has_value() ? std::optional{*id} : std::nullopt;
}

// The vocabulary GROWS: every margin, open-interest and self-trade check adds
// a reason glued in front of a base state ("openInterestCapCanceled"). Matching
// the tail keeps next month's word out of the unknown bucket, where query_order
// would refuse to speak about an order the venue plainly calls dead.
std::optional<OrderStatus> status_from(std::string_view text, bool partial) noexcept {
    if (text == "open") {
        return partial ? OrderStatus::PartiallyFilled : OrderStatus::Accepted;
    }
    if (text == "filled") {
        return OrderStatus::Filled;
    }
    if (text == "triggered") {
        return OrderStatus::Triggered;
    }
    if (text == "canceled" || text.ends_with("Canceled") || text.ends_with("Cancel")) {
        return OrderStatus::Canceled;
    }
    if (text == "rejected" || text.ends_with("Rejected")) {
        return OrderStatus::Rejected;
    }
    return std::nullopt;
}

// Everything both order reads share.
std::optional<exec::OrderStatusReport> read_order(const WireOpenOrder& wire,
                                                  const exec::InstrumentTable& table) {
    // A conditional is DORMANT: `limitPx` is where it WILL rest once triggerPx
    // prints. The domain cannot say that, and reporting it as a live limit
    // tells a reconciler about an order that is not in the book. Only the
    // frontendOpenOrders form carries the flag, which is why that form is read.
    if (wire.is_trigger) {
        return std::nullopt;
    }
    const auto* instrument = table.find(wire.coin);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto leaves = to_lots(wire.qty, base);
    // `sz` is what REMAINS and `origSz` what was sent; an absent original reads
    // as equal to the remainder rather than guessed, so a row that does not
    // carry it reports Accepted instead of an invented partial fill.
    const auto total = to_lots(wire.orig_qty.empty() ? wire.qty : wire.orig_qty, base);
    const auto order_id = id_from<ExchangeOrderId>(wire.oid);
    if (!leaves.has_value() || !total.has_value() || !order_id.has_value()) {
        return std::nullopt;
    }

    exec::OrderStatusReport out{};
    out.instrument = base.id;
    out.side = wire.buy ? Side::Buy : Side::Sell;
    out.exchange_order_id = *order_id;
    (void)cloid_to_ulid(wire.cloid, out.exchange_client_order_id);
    out.status = leaves->raw < total->raw ? OrderStatus::PartiallyFilled : OrderStatus::Accepted;
    if (const auto price = to_ticks(wire.price, base); price.has_value()) {
        out.price = *price;
    }
    out.qty = *total;
    out.leaves = *leaves;
    out.ts_venue = ms_to_unix(wire.ts_ms, UnixNanos{});
    return out;
}

} // namespace

std::optional<PriceTicks> price_from_wire(std::string_view text, const InstrumentBase& base) {
    return to_ticks(text, base);
}

std::optional<QtyLots> qty_from_wire(std::string_view text, const InstrumentBase& base) {
    return to_lots(text, base);
}

std::int64_t spot_asset_id(std::int64_t pair_index) noexcept {
    return k_spot_asset_offset + pair_index;
}

std::string_view cloid_text(const ExchangeClientOrderId& id, std::span<char> out) noexcept {
    const auto text = id.view();
    if (text.size() != k_ulid_chars || out.size() < k_cloid_chars) {
        return {};
    }
    // 26 base-32 digits span 130 bits, two more than the number underneath: a
    // ULID's 48-bit millisecond leaves them clear, so the leading digit never
    // exceeds 7. Checked BEFORE the accumulation, because those two bits are
    // exactly the ones the shift drops — by the end the overflow is invisible.
    if (crockford_value(text.front()) >= 8) {
        return {};
    }
    uint128_t value = 0;
    for (const char c : text) {
        const auto digit = crockford_value(c);
        if (digit < 0) {
            return {};
        }
        value = (value << 5) | static_cast<uint128_t>(digit);
    }
    out[0] = '0';
    out[1] = 'x';
    constexpr std::string_view k_hex = "0123456789abcdef";
    for (std::size_t i = 0; i < 32; ++i) {
        const auto nibble = static_cast<std::size_t>((value >> ((31 - i) * 4)) & 0xFU);
        out[2 + i] = k_hex[nibble];
    }
    return std::string_view{out.data(), k_cloid_chars};
}

bool cloid_to_ulid(std::string_view cloid, ExchangeClientOrderId& out) noexcept {
    if (cloid.size() != k_cloid_chars || cloid[0] != '0' || (cloid[1] != 'x' && cloid[1] != 'X')) {
        return false;
    }
    uint128_t value = 0;
    for (const char c : cloid.substr(2)) {
        const auto digit = hex_value(c);
        if (digit < 0) {
            return false;
        }
        value = (value << 4) | static_cast<uint128_t>(digit);
    }
    std::array<char, k_ulid_chars> text{};
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto shift = (text.size() - 1 - i) * 5;
        text[i] = k_crockford[static_cast<std::size_t>((value >> shift) & 0x1FU)];
    }
    const auto id = ExchangeClientOrderId::from(std::string_view{text.data(), text.size()});
    if (!id.has_value()) {
        return false;
    }
    out = *id;
    return true;
}

std::optional<Instrument> map_spot_instrument(const WireSpotAsset& wire) {
    // DERIVED, NOT VENUE-STATED — and from the BASE TOKEN's szDecimals, which
    // is the only place the pair's precision is written down.
    const auto lot = step_from_decimals(wire.sz_decimals);
    const auto tick = step_from_decimals(k_spot_price_digits - wire.sz_decimals);
    if (!lot.has_value() || !tick.has_value()) {
        return std::nullopt;
    }
    // The wire coin, "@<index>" for every pair but the canonical PURR/USDC. A
    // table keyed by the human name matches nothing the venue ever sends.
    const auto symbol = SymbolText::from(wire.name);
    if (!symbol.has_value()) {
        return std::nullopt;
    }
    // The readable name is BOTH token names, because the quote is not always
    // USDC. HYPE lists against USDC, USDT0, USDH and USDE — four books at four
    // prices — and naming them all "HYPE/USDC" hands a board joining venues on
    // local_symbol four candidates for one leg with no way to choose. The wire
    // `name` cannot stand in: it is an index no sibling catalogue would know.
    std::array<char, sizeof(SymbolText::data)> local{};
    if (wire.base.empty() || wire.quote.empty() ||
        wire.base.size() + 1 + wire.quote.size() > local.size()) {
        return std::nullopt;
    }
    std::size_t at = 0;
    for (const char c : wire.base) {
        local[at++] = c;
    }
    local[at++] = '/';
    for (const char c : wire.quote) {
        local[at++] = c;
    }
    const auto local_symbol = SymbolText::from(std::string_view{local.data(), at});
    if (!local_symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // Provisional: InstrumentTable::add assigns the SymbolId that indexes it.
    base.id = InstrumentId::make(VenueId::Hyperliquid, MarketType::Spot, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *local_symbol;
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.contract_value = *parse_decimal_e9("1"); // spot sizes are base units
    base.kind = ContractKind::Linear;
    return SpotInstrument{base};
}

std::optional<exec::PositionReport>
map_position(const WirePosition& wire, const exec::InstrumentTable& table, UnixNanos ts_venue) {
    const auto* instrument = table.find(wire.coin);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    // Signed, with no side field beside it to disagree with: a reader that
    // takes the magnitude reports every short as a long.
    const auto size = to_float_lots(wire.size, base);
    if (!size.has_value()) {
        return std::nullopt;
    }

    exec::PositionReport out{};
    out.instrument = base.id;
    out.net_qty = *size;
    // A weighted average need not land on the tick grid, so an unconvertible
    // entry is left undefined rather than sinking the position it belongs to.
    if (const auto entry = to_ticks(wire.entry_px, base); entry.has_value()) {
        out.avg_entry = *entry;
    }
    out.ts_venue = ts_venue;
    return out;
}

std::optional<exec::FillReport> map_fill(const WireFill& wire, const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.coin);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto price = to_ticks(wire.price, base);
    const auto qty = to_lots(wire.qty, base);
    const auto exec_id = id_from<ExecId>(wire.tid);
    const auto order_id = id_from<ExchangeOrderId>(wire.oid);
    if (!price.has_value() || !qty.has_value() || !exec_id.has_value() || !order_id.has_value()) {
        return std::nullopt;
    }

    exec::FillReport out{};
    out.exec_id = *exec_id;
    out.exchange_order_id = *order_id;
    (void)cloid_to_ulid(wire.cloid, out.exchange_client_order_id);
    out.instrument = base.id;
    out.side = wire.buy ? Side::Buy : Side::Sell;
    out.price = *price;
    out.qty = *qty;
    out.fee = fee_e9(wire.fee);
    // `crossed` is the whole liquidity signal: true means we took. There is no
    // third spelling, so Unknown is never the honest answer here.
    out.liquidity = wire.crossed ? Liquidity::Taker : Liquidity::Maker;
    out.ts_venue = ms_to_unix(wire.ts_ms, UnixNanos{});
    return out;
}

std::optional<exec::OrderStatusReport> map_open_order(const WireOpenOrder& wire,
                                                      const exec::InstrumentTable& table) {
    return read_order(wire, table);
}

std::optional<exec::OrderStatusReport> map_order_status(const WireOrderStatus& wire,
                                                        const exec::InstrumentTable& table) {
    if (!wire.found) {
        return std::nullopt; // "unknownOid": forgotten and never minted read alike
    }
    auto out = read_order(wire.order, table);
    if (!out.has_value()) {
        return std::nullopt;
    }
    const auto status = status_from(wire.status, out->leaves.raw < out->qty.raw);
    if (!status.has_value()) {
        return std::nullopt;
    }
    out->status = *status;
    // A terminal order rests nowhere. Leaving `leaves` at the size the venue
    // last saw would tell a reconciler there is still work in the book.
    if (is_terminal(*status)) {
        out->leaves = QtyLots::from_raw(0);
    }
    out->ts_venue = ms_to_unix(wire.ts_ms, out->ts_venue);
    return out;
}

std::optional<LedgerEntry> map_funding_charge(const WireFundingCharge& wire,
                                              const exec::InstrumentTable& table,
                                              UnixNanos ts_init) {
    if (wire.type != "funding") {
        return std::nullopt;
    }
    const auto* instrument = table.find(wire.coin);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    // VERBATIM. The venue signs `usdc` itself — negative was paid away — and
    // `szi` beside it only restates which way the position pointed. Taking a
    // magnitude here would report every payment as income.
    const auto amount = to_amount_e9(wire.usdc);
    if (!amount.has_value()) {
        return std::nullopt; // an unreadable figure is not a zero charge
    }

    LedgerEntry charge{};
    charge.instrument = base_of(*instrument).id;
    charge.amount = *amount;
    charge.kind = LedgerKind::Funding;
    charge.settled_at = ms_to_unix(wire.time_ms, UnixNanos{});
    // Built rather than carried — map.hpp says why the venue's `hash` cannot
    // be. Funding settles once an hour per coin, so the pair is unique, and it
    // is the same text on every replay of the same page.
    std::array<char, 48> key{};
    if (wire.coin.size() + 2 <= key.size()) {
        auto* cursor = std::copy(wire.coin.begin(), wire.coin.end(), key.data());
        *cursor++ = ':';
        if (const auto [stop, ec] = std::to_chars(cursor, key.data() + key.size(), wire.time_ms);
            ec == std::errc{}) {
            const std::string_view text{key.data(), static_cast<std::size_t>(stop - key.data())};
            if (const auto id = ExecId::from(text); id.has_value()) {
                charge.reference = *id;
            }
        }
    }
    charge.ts_init = ts_init;
    return charge;
}

} // namespace raikiri::venues::hyperliquid
