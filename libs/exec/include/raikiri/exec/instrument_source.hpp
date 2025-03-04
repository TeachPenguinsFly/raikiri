#pragma once

// Cold reference-data pull: the venue's full instrument catalog, already
// converted to fixed-point and validated. Feeds symbol interning, the
// in-memory instrument tables, and Postgres. Never on any hot path.

#include <raikiri/domain/instrument.hpp>
#include <raikiri/exec/error.hpp>

#include <expected>
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

} // namespace raikiri::exec
