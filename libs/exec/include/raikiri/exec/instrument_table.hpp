#pragma once

// Symbol interning and instrument lookup for one venue.
//
// The table is immutable after load. Instruments get delisted and added, but
// that is a restart or an explicit reload — never a mutation racing with the
// hot loop, which is what makes lock-free reads safe here without any
// synchronisation at all.

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/instrument.hpp>

#include <cstdint>
#include <deque>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace raikiri::exec {

enum class TableError : std::uint8_t {
    DuplicateSymbol,
    InvalidInstrument, // failed domain validation (zero tick/lot, empty symbol)
    Full,              // more instruments than SymbolId space allows in practice
};

[[nodiscard]] std::string_view to_string(TableError err) noexcept;

class InstrumentTable {
public:
    InstrumentTable() = default;

    // Assigns the next SymbolId and takes ownership.
    [[nodiscard]] std::expected<InstrumentId, TableError> add(Instrument instrument);

    // Hot path: inbound frames carry the venue's own symbol text.
    [[nodiscard]] const Instrument* find(std::string_view exchange_symbol) const noexcept;

    // Warm path: outbound orders need the text back.
    [[nodiscard]] const Instrument* find(InstrumentId id) const noexcept;

    [[nodiscard]] std::string_view exchange_symbol(InstrumentId id) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return instruments_.size(); }

    [[nodiscard]] std::span<const Instrument> all() const noexcept { return instruments_; }

    void reserve(std::size_t count);

private:
    std::vector<Instrument> instruments_; // indexed by SymbolId
    std::deque<std::string> exchange_symbols_;
    std::unordered_map<std::string_view, std::uint32_t> by_exchange_symbol_;
};

} // namespace raikiri::exec
