#pragma once

// Per-strategy id mint. One IdGen per strategy instance, single-threaded by
// design (the OMS owns it): a plain counter for ClientOrderId keeps the
// {strategy_id | seq} handle dense and journal-replayable, and one UlidGen
// keeps ExchangeClientOrderIds monotone within the process — see ulid.hpp for
// why monotonicity matters. Seq starts at 1 so raw 0 stays the universal
// "no order" sentinel (ClientOrderId::valid()).

#include <raikiri/common/time.hpp>
#include <raikiri/common/ulid.hpp>
#include <raikiri/domain/identifiers.hpp>

#include <cstdint>

namespace raikiri::oms {

class IdGen {
public:
    // Seeds the ULID generator from OS entropy (cold path, may touch the OS).
    explicit IdGen(std::uint16_t strategy_id) : strategy_id_{strategy_id} {}

    // Deterministic seed for tests and simulation.
    IdGen(std::uint16_t strategy_id, std::uint64_t ulid_seed) noexcept
        : ulid_gen_{ulid_seed}, strategy_id_{strategy_id} {}

    [[nodiscard]] ClientOrderId next_client_id() noexcept {
        return ClientOrderId::make(strategy_id_, ++seq_);
    }

    [[nodiscard]] ExchangeClientOrderId next_exchange_client_id(UnixNanos wall) noexcept {
        return ulid_gen_.next(wall);
    }

    [[nodiscard]] std::uint16_t strategy_id() const noexcept { return strategy_id_; }

private:
    UlidGen ulid_gen_{};
    std::uint16_t strategy_id_{0};
    std::uint64_t seq_{0};
};

} // namespace raikiri::oms
