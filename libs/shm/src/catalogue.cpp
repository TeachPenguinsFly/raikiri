#include <raikiri/shm/catalogue.hpp>

namespace raikiri::shm {

std::expected<void, CatalogueError> CatalogueTable::push(const Instrument& instrument) noexcept {
    const auto slot = count_.load(std::memory_order_relaxed);
    if (slot >= k_catalogue_cells) {
        return std::unexpected{CatalogueError::Full};
    }
    if (base_of(instrument).id.symbol().raw != slot) {
        return std::unexpected{CatalogueError::SlotMismatch};
    }
    entries_[slot] = instrument;
    count_.store(slot + 1, std::memory_order_release);
    return {};
}

const Instrument* CatalogueTable::find(InstrumentId id) const noexcept {
    const auto slot = id.symbol().raw;
    if (slot >= size()) {
        return nullptr;
    }
    const auto* entry = &entries_[slot];
    return base_of(*entry).id == id ? entry : nullptr;
}

} // namespace raikiri::shm
