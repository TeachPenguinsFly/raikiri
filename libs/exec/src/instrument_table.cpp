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
    exchange_symbols_.reserve(count);
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
    if (by_exchange_symbol_.contains(symbol_text)) {
        return std::unexpected{TableError::DuplicateSymbol};
    }

    const auto index = static_cast<std::uint32_t>(instruments_.size());
    // The id is rewritten rather than trusted: the loader that built this
    // instrument knew the venue and market but not which slot it would land
    // in, and SymbolId IS that slot.
    const auto id = InstrumentId::make(base.id.venue(), base.id.market(), SymbolId{index});
    std::visit([id](auto& concrete) { concrete.base.id = id; }, instrument);

    instruments_.push_back(std::move(instrument));
    exchange_symbols_.push_back(symbol_text);
    // Keyed on the owned copy, which reserve() keeps stable for the life of
    // the table.
    by_exchange_symbol_.emplace(exchange_symbols_.back(), index);
    return id;
}

const Instrument* InstrumentTable::find(std::string_view exchange_symbol) const noexcept {
    const auto it = by_exchange_symbol_.find(exchange_symbol);
    return it == by_exchange_symbol_.end() ? nullptr : &instruments_[it->second];
}

const Instrument* InstrumentTable::find(InstrumentId id) const noexcept {
    const auto index = id.symbol().raw;
    return index < instruments_.size() ? &instruments_[index] : nullptr;
}

std::string_view InstrumentTable::exchange_symbol(InstrumentId id) const noexcept {
    const auto index = id.symbol().raw;
    return index < exchange_symbols_.size() ? std::string_view{exchange_symbols_[index]}
                                            : std::string_view{};
}

} // namespace raikiri::exec
