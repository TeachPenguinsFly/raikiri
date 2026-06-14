#pragma once

// Cold reference-data pull: the venue's full instrument catalog, already
// converted to fixed-point and validated. Feeds symbol interning, the
// in-memory instrument tables, and Postgres. Never on any hot path.

#include <raikiri/domain/instrument.hpp>
#include <raikiri/exec/error.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <vector>

namespace raikiri::exec {

class InstrumentSource {
public:
    InstrumentSource() = default;
    InstrumentSource(const InstrumentSource&) = delete;
    InstrumentSource& operator=(const InstrumentSource&) = delete;
    virtual ~InstrumentSource() = default;

    [[nodiscard]] virtual std::expected<std::vector<Instrument>, VenueError>
    fetch_all(MarketType market) = 0;
};

struct CatalogueBackoff {
    std::chrono::milliseconds first{1'000};
    std::chrono::milliseconds cap{60'000};
};

// Retries fetch_all until it succeeds, fails in a way no retry can fix
// (Terminal, AuthFailure), or `stopping` turns true; the last two return the
// error. `on_retry` hears each failure that will be retried, with its attempt
// number and the wait before the next one. `stopping` also abandons a request
// in flight, through an AbandonScope held for the whole load.
[[nodiscard]] std::expected<std::vector<Instrument>, VenueError> load_catalogue(
    InstrumentSource& source,
    MarketType market,
    const std::function<bool()>& stopping,
    const std::function<void(const VenueError&, std::uint32_t, std::chrono::milliseconds)>&
        on_retry,
    CatalogueBackoff backoff = {});

} // namespace raikiri::exec
