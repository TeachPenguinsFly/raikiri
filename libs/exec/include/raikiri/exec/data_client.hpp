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

#include <cstddef>
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

    // BLOCKS IN THE REACTOR, and it exists because parking beside one is what
    // the gateway's processor was actually going on.
    //
    // poll() returns whether or not a socket was ready, so a loop built on it
    // alone has to sleep between passes, and that sleep is a fixed cost paid
    // per pass rather than per message. Measured on okx: one symbol burned
    // 35.5% of a core and the entire 471-instrument catalogue — ninety-four
    // times the messages — burned 37.8%. Confirmed again on 2026-09-18, when
    // dropping a whole channel cut packets 12% and freed no processor at all.
    //
    // This waits inside epoll instead: an idle venue costs nothing, and a
    // busy one wakes on the packet rather than on the next tick of a timer.
    //
    // ONLY WHERE NOTHING ELSE MUST BE NOTICED. The caller's command rings are
    // shared memory and cannot wake a reactor, so a gateway that accepts
    // orders must keep parking until they can — see gatewayd's loop. Returns
    // handlers run; zero from an adapter with no reactor of its own.
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

private:
    [[nodiscard]] static std::expected<void, VenueError> unsupported(SubKind kind,
                                                                     InstrumentId) noexcept {
        return std::unexpected{VenueError::make(ErrorClass::Terminal, 0, to_string(kind))};
    }
};

} // namespace raikiri::exec
