#include <raikiri/exec/instrument_table.hpp>

namespace raikiri::exec {

namespace {

// SymbolId is 32 bits, but the practical ceiling is the shm BBO table, which
// is indexed by SymbolId and sized in cache lines. Refusing here keeps the
// failure at catalogue load rather than at the first tick for symbol 4097.
constexpr std::size_t k_max_instruments = 4096;

} // namespace

std::string_view to_string(TableError err) noexcept {
    switch (err) {
    case TableError::DuplicateSymbol:
        return "duplicate exchange symbol";
    case TableError::InvalidInstrument:
        return "invalid instrument";
    case TableError::Full:
        return "instrument table full";
    }
    return "unknown";
}

void InstrumentTable::reserve(std::size_t count) {
    instruments_.reserve(count);
    by_exchange_symbol_.reserve(count);
}

std::expected<InstrumentId, TableError> InstrumentTable::add(Instrument instrument) {
    const auto& base = base_of(instrument);
    if (!validate(base)) {
        return std::unexpected{TableError::InvalidInstrument};
    }
    if (instruments_.size() >= k_max_instruments) {
        return std::unexpected{TableError::Full};
    }
    const auto symbol_text = std::string{base.exchange_symbol.view()};
    // A duplicate is the same symbol in the same MARKET. Refusing on the symbol
    // alone silently dropped every spot instrument on a venue whose perpetual
    // shares the ticker — which is most of them — and left the gateway holding
    // whichever catalogue happened to load first.
    if (const auto it = by_exchange_symbol_.find(symbol_text); it != by_exchange_symbol_.end()) {
        for (const auto slot : it->second) {
            if (base_of(instruments_[slot]).id.market() == base.id.market()) {
                return std::unexpected{TableError::DuplicateSymbol};
            }
        }
    }

    const auto index = static_cast<std::uint32_t>(instruments_.size());
    const auto id = InstrumentId::make(base.id.venue(), base.id.market(), SymbolId{index});
    std::visit([id](auto& concrete) { concrete.base.id = id; }, instrument);

    instruments_.push_back(std::move(instrument));
    exchange_symbols_.push_back(symbol_text);
    by_exchange_symbol_[exchange_symbols_.back()].push_back(index);
    return id;
}

const Instrument* InstrumentTable::find(std::string_view exchange_symbol) const noexcept {
    const auto it = by_exchange_symbol_.find(exchange_symbol);
    if (it == by_exchange_symbol_.end() || it->second.size() != 1) {
        return nullptr; // absent, or ambiguous across markets — never a guess
    }
    return &instruments_[it->second.front()];
}

const Instrument* InstrumentTable::find(std::string_view exchange_symbol,
                                        MarketType market) const noexcept {
    const auto it = by_exchange_symbol_.find(exchange_symbol);
    if (it == by_exchange_symbol_.end()) {
        return nullptr;
    }
    for (const auto slot : it->second) {
        if (base_of(instruments_[slot]).id.market() == market) {
            return &instruments_[slot];
        }
    }
    return nullptr;
}

const Instrument* InstrumentTable::find(InstrumentId id) const noexcept {
    const auto index = id.symbol().raw;
    if (index >= instruments_.size()) {
        return nullptr;
    }
    // The WHOLE id is compared, not just the index it carries. Spot and
    // perpetual instruments share one index space here, so an id built with the
    // wrong market — a hand-made one, or a bug upstream — otherwise resolves to
    // a real instrument that is simply not the one asked for, and every price
    // and quantity derived from it is silently scaled by another contract's
    // steps. shm::CatalogueTable::find has always compared the whole id; this
    // is the same rule on this side of the boundary.
    const auto* entry = &instruments_[index];
    return base_of(*entry).id == id ? entry : nullptr;
}

std::string_view InstrumentTable::exchange_symbol(InstrumentId id) const noexcept {
    // Resolved through find() so a wrong-market id yields nothing rather than
    // another instrument's ticker — a mismatch the two answers must not disagree
    // about, since a caller commonly asks for both.
    const auto index = id.symbol().raw;
    return find(id) != nullptr ? std::string_view{exchange_symbols_[index]} : std::string_view{};
}

} // namespace raikiri::exec
