#include <raikiri/exec/instrument_table.hpp>

namespace raikiri::exec {

namespace {

// the shm bbo table, indexed by SymbolId, caps the practical count
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
    // a duplicate is the same symbol in the same market
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

    instruments_.push_back(instrument);
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

const Instrument* InstrumentTable::find(std::string_view exchange_symbol, MarketType market) const noexcept {
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
    // compare the whole id; spot and perp share one index space
    const auto* entry = &instruments_[index];
    return base_of(*entry).id == id ? entry : nullptr;
}

std::string_view InstrumentTable::exchange_symbol(InstrumentId id) const noexcept {
    // through find() so a wrong-market id yields nothing
    const auto index = id.symbol().raw;
    return find(id) != nullptr ? std::string_view{exchange_symbols_[index]} : std::string_view{};
}

} // namespace raikiri::exec
