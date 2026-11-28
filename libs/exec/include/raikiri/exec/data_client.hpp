#pragma once

// market-data side of a venue adapter; unimplemented kinds report unsupported

#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/exec/error.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace raikiri::exec {

enum class SubKind : std::uint8_t { Bbo = 0, Trades, Depth, Funding, MarkPrice, Kline };

[[nodiscard]] std::string_view to_string(SubKind kind) noexcept;

class DataClient {
public:
    DataClient() = default;
    DataClient(const DataClient&) = delete;
    DataClient& operator=(const DataClient&) = delete;
    virtual ~DataClient() = default;

    virtual std::expected<void, VenueError> start() = 0;
    virtual void stop() = 0;
    [[nodiscard]] virtual bool connected() const noexcept = 0;

    // subscriptions the venue refused; zero from adapters that do not count them
    [[nodiscard]] virtual std::uint64_t subscribe_refusals() const noexcept { return 0; }

    virtual void poll(UnixNanos now) noexcept { (void)now; }

    // blocks in the reactor up to `timeout_ms`; returns handlers run, zero without a reactor
    // shm command rings cannot wake it, so a gateway taking orders keeps polling
    virtual std::size_t wait(std::uint32_t timeout_ms) noexcept {
        (void)timeout_ms;
        return 0;
    }

    virtual std::expected<void, VenueError> subscribe(SubKind kind, InstrumentId instrument) {
        return unsupported(kind, instrument);
    }

    virtual std::expected<void, VenueError> unsubscribe(SubKind kind, InstrumentId instrument) {
        return unsupported(kind, instrument);
    }

    // a top-of-book stream says nothing until a top moves, so a quiet market stays unpriced after a (re)subscribe;
    // venues whose rest answers every market's touch in one call fill those once through on_touch_seed
    [[nodiscard]] virtual bool seeds_touch() const noexcept { return false; }

    // one bulk read for `unpriced`; false when refused, which leaves the pass to the caller's retry
    virtual bool seed_touch(std::span<const InstrumentId> unpriced) {
        (void)unpriced;
        return false;
    }

private:
    [[nodiscard]] static std::expected<void, VenueError> unsupported(SubKind kind, InstrumentId) noexcept {
        return std::unexpected{VenueError::make(ErrorClass::Terminal, 0, to_string(kind))};
    }
};

} // namespace raikiri::exec
