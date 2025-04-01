#pragma once

// The instrument catalogue channel.
//
// SymbolId is a dense slot index the gateway hands out while interning the
// venue's catalogue, and it rides inside InstrumentId. Both processes must
// use the same numbering: an order for symbol 41 against a different table is
// a well-formed order for the wrong contract, which nothing downstream can
// catch. So the consumer adopts the gateway's numbering instead of deriving
// its own.
//
// Written once before publish() and immutable after, so an attached consumer
// always sees a complete table and needs no synchronisation to read it. New
// listings mean a gateway restart under a fresh generation.

#include <raikiri/domain/instrument.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <expected>
#include <span>
#include <type_traits>

namespace raikiri::shm {

// Must match the BBO table's cell count (asserted in records.hpp): an
// instrument past that bound would have nowhere to quote from.
inline constexpr std::size_t k_catalogue_cells = 4096;

static_assert(std::is_trivially_copyable_v<Instrument>);

enum class CatalogueError : std::uint8_t {
    Full,
    SlotMismatch, // the instrument's SymbolId is not the slot it would occupy
};

class CatalogueTable {
public:
    // Owner side, before publish(). Slot order IS the numbering, so an
    // instrument that disagrees with its slot is refused, not renumbered.
    [[nodiscard]] std::expected<void, CatalogueError> push(const Instrument& instrument) noexcept;

    [[nodiscard]] std::uint32_t size() const noexcept {
        return count_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::span<const Instrument> all() const noexcept {
        return {entries_.data(), size()};
    }

    // Null when the id names an unpublished slot, or a venue or market other
    // than the one that slot holds.
    [[nodiscard]] const Instrument* find(InstrumentId id) const noexcept;

private:
    alignas(64) std::atomic<std::uint32_t> count_{0};
    std::array<Instrument, k_catalogue_cells> entries_{};
};

} // namespace raikiri::shm
