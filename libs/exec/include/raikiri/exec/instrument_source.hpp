#pragma once

// cold pull of the venue's full instrument catalogue, converted and validated

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

// retries fetch_all until success, a Terminal or AuthFailure error, or `stopping`
// `stopping` also abandons the request in flight
[[nodiscard]] std::expected<std::vector<Instrument>, VenueError> load_catalogue(
    InstrumentSource& source,
    MarketType market,
    const std::function<bool()>& stopping,
    const std::function<void(const VenueError&, std::uint32_t, std::chrono::milliseconds)>&
        on_retry,
    CatalogueBackoff backoff = {});

} // namespace raikiri::exec
