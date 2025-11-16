#pragma once

// Market-data side of a venue adapter.
//
// One method per subscription kind, each defaulting to a body that reports
// "unsupported" — an adapter implements exactly the kinds its venue offers and
// inherits honest failure for the rest. The gateway core (not the adapter)
// owns refcounting and resubscribe-on-reconnect; adapters only translate
// subscribe intents into venue wire messages.

#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/exec/error.hpp>

#include <cstdint>
#include <expected>

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

    // SUBSCRIPTIONS THE VENUE REFUSED. `connected()` answers whether a socket
    // is up and nothing about whether it is carrying what was asked for — a
    // venue that answered a third of the catalogue with a refusal and the rest
    // with silence reports exactly the same `true`.
    //
    // Zero from an adapter that does not count them, which is indistinguishable
    // from none refused and is why coverage is measured beside this rather than
    // inferred from it.
    [[nodiscard]] virtual std::uint64_t subscribe_refusals() const noexcept { return 0; }

    virtual void poll(UnixNanos now) noexcept { (void)now; }

    virtual std::expected<void, VenueError> subscribe(SubKind kind, InstrumentId instrument) {
        return unsupported(kind, instrument);
    }

    virtual std::expected<void, VenueError> unsubscribe(SubKind kind, InstrumentId instrument) {
        return unsupported(kind, instrument);
    }

private:
    [[nodiscard]] static std::expected<void, VenueError> unsupported(SubKind kind,
                                                                     InstrumentId) noexcept {
        return std::unexpected{VenueError::make(ErrorClass::Terminal, 0, to_string(kind))};
    }
};

} // namespace raikiri::exec
