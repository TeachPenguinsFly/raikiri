#pragma once

#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/instrument.hpp>

#include <span>
#include <string_view>

namespace raikiri {

[[nodiscard]] std::string_view to_string(AssetClass cls) noexcept;

struct CuratedRow {
    VenueId venue;
    std::string_view symbol;
    AssetClass cls;
    std::string_view underlying;
};

[[nodiscard]] std::span<const CuratedRow> curated_rows() noexcept;

// True for a venue whose adapter reads the class off the venue's own catalogue
// field. There an Unknown the adapter left stays Unknown; elsewhere it defaults
// to Crypto.
[[nodiscard]] bool states_asset_class(VenueId venue) noexcept;

// Settles `asset_class` and `underlying` once the adapter has mapped the row:
// the curated table first, then the adapter's own verdict, then the default. A
// dex-prefixed name ("xyz:TSLA") never defaults — the prefix is there because
// the bare ticker means something else on the main dex.
void classify(VenueId venue, InstrumentBase& base) noexcept;

inline void classify(VenueId venue, Instrument& instrument) noexcept {
    std::visit([venue](auto& i) { classify(venue, i.base); }, instrument);
}

} // namespace raikiri
