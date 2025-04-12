#pragma once

// Symbol interning and instrument lookup for one venue.
//
// This is where venue strings stop existing. A gateway resolves an exchange
// symbol to a dense SymbolId exactly once — when the catalogue loads — and
// every later message carries the integer. The predecessor system did the
// opposite, hashing a string on every inbound tick, and that lookup was its
// single most-executed line of code.
//
// Two directions are needed and they are not symmetric:
//   exchange symbol -> InstrumentId   on every inbound frame (hot)
//   InstrumentId    -> exchange symbol on every outbound order (warm)
// The first is a hash lookup; the second is an array index, because
// InstrumentId embeds the SymbolId that indexes this table.
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

    // Assigns the next SymbolId and takes ownership. The instrument must
    // already be validated: an instrument with a zero tick size divides by
    // zero at order time, so it is refused here rather than discovered then.
    [[nodiscard]] std::expected<InstrumentId, TableError> add(Instrument instrument);

    // Hot path: inbound frames carry the venue's own symbol text.
    [[nodiscard]] const Instrument* find(std::string_view exchange_symbol) const noexcept;

    // Warm path: outbound orders need the text back.
    [[nodiscard]] const Instrument* find(InstrumentId id) const noexcept;

    [[nodiscard]] std::string_view exchange_symbol(InstrumentId id) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return instruments_.size(); }

    [[nodiscard]] std::span<const Instrument> all() const noexcept { return instruments_; }

    // Purely an optimization: it saves rehashing mid-load. It used to be a
    // correctness requirement too, because the key views pointed into a
    // vector that reallocated — three separate venue adapters were caught by
    // that as a heap-use-after-free. The key storage is now node-stable, so
    // forgetting this call costs time and nothing else.
    void reserve(std::size_t count);

private:
    std::vector<Instrument> instruments_; // indexed by SymbolId
    // Deque, not vector: references to existing elements survive every
    // push_back, which is what makes the views below safe by construction
    // rather than by the caller remembering to reserve.
    std::deque<std::string> exchange_symbols_;
    std::unordered_map<std::string_view, std::uint32_t> by_exchange_symbol_;
};

} // namespace raikiri::exec
