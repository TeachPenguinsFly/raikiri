#pragma once

#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/margin.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace raikiri::venues::okx {

// the margin mode chosen per instrument through admind; okx takes it per order as tdMode, cross unless chosen.
// one table per gatewayd, written by the poller's adapter and read by the order adapter's builder.
// held in memory only: after a restart every instrument trades cross again until isolated is chosen anew
class TradeModes {
public:
    static constexpr std::size_t k_capacity = 64;

    [[nodiscard]] static TradeModes& process() noexcept {
        static TradeModes modes;
        return modes;
    }

    [[nodiscard]] MarginMode mode(InstrumentId instrument) const noexcept {
        if (isolated_.load(std::memory_order_acquire) == 0) {
            return MarginMode::Cross;
        }
        for (const auto& slot : slots_) {
            if (slot.load(std::memory_order_acquire) == instrument.packed) {
                return MarginMode::Isolated;
            }
        }
        return MarginMode::Cross;
    }

    // one writer; false when the table is full
    bool set(InstrumentId instrument, MarginMode mode) noexcept {
        const bool isolated = mode == MarginMode::Isolated;
        for (auto& slot : slots_) {
            if (slot.load(std::memory_order_relaxed) == instrument.packed) {
                if (!isolated) {
                    slot.store(0, std::memory_order_release);
                    isolated_.fetch_sub(1, std::memory_order_release);
                }
                return true;
            }
        }
        if (!isolated) {
            return true;
        }
        for (auto& slot : slots_) {
            if (slot.load(std::memory_order_relaxed) == 0) {
                slot.store(instrument.packed, std::memory_order_release);
                isolated_.fetch_add(1, std::memory_order_release);
                return true;
            }
        }
        return false;
    }

private:
    std::array<std::atomic<std::uint64_t>, k_capacity> slots_{};
    std::atomic<std::uint32_t> isolated_{0};
};

} // namespace raikiri::venues::okx
