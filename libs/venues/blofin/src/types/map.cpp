#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/blofin/map.hpp>

#include <charconv>
#include <optional>

namespace raikiri::venues::blofin {

namespace {

std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

std::optional<FixedE9> positive(std::string_view text) {
    const auto value = parse_decimal_e9(text);
    if (!value.has_value() || value->raw <= 0) {
        return std::nullopt;
    }
    return *value;
}

AssetClass asset_class_of(std::string_view text) noexcept {
    if (text == "Crypto") {
        return AssetClass::Crypto;
    }
    if (text == "Stocks") {
        return AssetClass::Equity;
    }
    if (text == "Commodities") {
        return AssetClass::Commodity;
    }
    if (text == "Indices") {
        return AssetClass::Index;
    }
    return AssetClass::Unknown;
}

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * 1'000'000ULL};
}

std::uint32_t interval_of(std::string_view count, std::string_view unit) noexcept {
    std::uint32_t n = 0;
    const auto* const end = count.data() + count.size();
    const auto [stop, ec] = std::from_chars(count.data(), end, n);
    if (count.empty() || ec != std::errc{} || stop != end || n == 0 || n > 24 * 60) {
        return 0;
    }
    if (unit == "hour") {
        return n * 3600U;
    }
    if (unit == "minute") {
        return n * 60U;
    }
    return 0;
}

std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market) {
    if (market != MarketType::Perp || wire.inst_type != "SWAP" || wire.state != "live") {
        return std::nullopt;
    }
    const auto tick = positive(wire.tick_size);
    const auto lot = positive(wire.lot_size);
    const auto value = positive(wire.contract_value);
    const auto symbol = SymbolText::from(wire.inst_id);
    const auto local = canonical_symbol(wire.base_ccy, wire.quote_ccy);
    if (!tick.has_value() || !lot.has_value() || !value.has_value() || !symbol.has_value() || !local.has_value()) {
        return std::nullopt;
    }
    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Blofin, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *local;
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.contract_value = *value;
    if (wire.contract_type == "linear") {
        base.kind = ContractKind::Linear;
    } else if (wire.contract_type == "inverse") {
        base.kind = ContractKind::Inverse;
    } else {
        return std::nullopt;
    }
    base.asset_class = asset_class_of(wire.asset_class);
    return PerpInstrument{base, /*funding_interval_s=*/0};
}

void apply_schedule(PerpInstrument& perp, const WireFundingRow& row) noexcept {
    perp.funding_interval_s = interval_of(row.interval, row.interval_unit);
    if (const auto cap = parse_decimal_e9(row.cap); cap.has_value()) {
        perp.funding_cap = *cap;
    }
    if (const auto floor = parse_decimal_e9(row.floor); floor.has_value()) {
        perp.funding_floor = *floor;
    }
}

std::optional<TradeTick> map_trade(const WirePrint& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto price = steps_of(wire.price, base.tick_size);
    const auto qty = steps_of(wire.size, base.lot_size);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }
    TradeTick out{};
    out.instrument = base.id;
    out.price = PriceTicks::from_raw(*price);
    out.qty = QtyLots::from_raw(*qty);
    out.aggressor = wire.taker_bought ? Side::Buy : Side::Sell;
    out.ts_event = ms_to_unix(wire.ts_ms, UnixNanos{});
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats> map_ticker_stats(const WireTicker& wire, const InstrumentBase& base, UnixNanos ts_init) {
    TickerStats out{};
    if (base.kind == ContractKind::Inverse) {
        // both columns count one-dollar contracts here
        const auto contracts = parse_amount_e9(wire.vol_contracts);
        if (!contracts.has_value() || base.contract_value.raw % k_e9 != 0) {
            return std::nullopt;
        }
        out.volume_24h_quote = turnover_e6(*contracts, base.contract_value);
    } else {
        const auto volume = parse_amount_e9(wire.vol_currency);
        if (!volume.has_value()) {
            return std::nullopt;
        }
        out.volume_24h_base = *volume;
        // no turnover column; the day's coins at the last print
        if (const auto last = parse_amount_e9(wire.last); last.has_value()) {
            out.volume_24h_quote = turnover_e6(*volume, *last);
        }
    }
    out.instrument = base.id;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

bool note_funding(FundingMemo& memo, const WireFundingRow& row) noexcept {
    // the rate carries up to twenty decimals of double noise; the exact reader would refuse it
    const auto rate = parse_rate_e9(row.funding_rate);
    if (!rate.has_value()) {
        return false;
    }
    memo.rate = *rate;
    if (row.funding_ms != 0) {
        memo.next_funding = ms_to_unix(row.funding_ms, UnixNanos{});
    }
    if (!row.interval.empty() || !row.interval_unit.empty()) {
        memo.interval_s = interval_of(row.interval, row.interval_unit);
    }
    return true;
}

bool note_mark(FundingMemo& memo, const WireMark& wire, const InstrumentBase& base) noexcept {
    const auto mark = steps_of(wire.mark_price, base.tick_size);
    if (!mark.has_value()) {
        return false;
    }
    memo.mark = PriceTicks::from_raw(*mark);
    // an index need not sit on the grid, so it is truncated to it
    if (const auto index = parse_decimal_e9(wire.index_price); index.has_value() && base.tick_size.raw > 0) {
        memo.index = PriceTicks::from_raw(index->raw / base.tick_size.raw);
    }
    return true;
}

FundingUpdate
merged(const InstrumentBase& base, const FundingMemo& memo, UnixNanos ts_event, UnixNanos ts_init) noexcept {
    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = memo.rate;
    out.mark_price = memo.mark;
    out.index_price = memo.index;
    out.next_funding_time = memo.next_funding;
    out.interval_s = memo.interval_s;
    out.ts_event = ts_event;
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::venues::blofin
