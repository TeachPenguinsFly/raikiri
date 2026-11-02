#pragma once

// per-strategy id mint, single-threaded; seq starts at 1 so raw 0 means no order

#include <raikiri/common/time.hpp>
#include <raikiri/common/ulid.hpp>
#include <raikiri/domain/identifiers.hpp>

#include <algorithm>
#include <cstdint>

namespace raikiri::oms {

class IdGen {
public:
    // Seeds the ULID generator from OS entropy (cold path, may touch the OS).
    explicit IdGen(std::uint16_t strategy_id) : strategy_id_{strategy_id} {}

    // Deterministic seed for tests and simulation.
    IdGen(std::uint16_t strategy_id, std::uint64_t ulid_seed) noexcept
        : ulid_gen_{ulid_seed}, strategy_id_{strategy_id} {}

    [[nodiscard]] ClientOrderId next_client_id() noexcept { return ClientOrderId::make(strategy_id_, ++seq_); }

    // an executor's child: it books to `book` and comes back to the executor
    [[nodiscard]] ClientOrderId next_child_id(std::uint16_t book) noexcept {
        return ClientOrderId::make_child(book, ++seq_);
    }

    // a restart must not mint an id an earlier process used, since postgres keys orders on it
    void start_after(std::uint64_t seq) noexcept { seq_ = seq; }

    [[nodiscard]] ExchangeClientOrderId next_exchange_client_id(UnixNanos wall) noexcept {
        return ulid_gen_.next(wall);
    }

    // a restart picks up past what the journal holds, or a new order takes an old order's id
    void resume_after(std::uint64_t seq) noexcept { seq_ = std::max(seq_, seq); }

    [[nodiscard]] std::uint16_t strategy_id() const noexcept { return strategy_id_; }

private:
    UlidGen ulid_gen_{};
    std::uint16_t strategy_id_{0};
    std::uint64_t seq_{0};
};

} // namespace raikiri::oms
