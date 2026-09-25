#pragma once

// the consumer adopts the gateway's SymbolId numbering

#include <raikiri/domain/instrument.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <expected>
#include <span>
#include <type_traits>

namespace raikiri::shm {

// must match the bbo table's cell count
inline constexpr std::size_t k_catalogue_cells = 4096;

static_assert(std::is_trivially_copyable_v<Instrument>);

enum class CatalogueError : std::uint8_t {
    Full,
    SlotMismatch, // the instrument's SymbolId is not the slot it would occupy
};

class CatalogueTable {
public:
    [[nodiscard]] std::expected<void, CatalogueError> push(const Instrument& instrument) noexcept;

    [[nodiscard]] std::uint32_t size() const noexcept { return count_.load(std::memory_order_acquire); }

    [[nodiscard]] std::span<const Instrument> all() const noexcept { return {entries_.data(), size()}; }

    [[nodiscard]] const Instrument* find(InstrumentId id) const noexcept;

private:
    alignas(64) std::atomic<std::uint32_t> count_{0};
    std::array<Instrument, k_catalogue_cells> entries_{};
};

} // namespace raikiri::shm
