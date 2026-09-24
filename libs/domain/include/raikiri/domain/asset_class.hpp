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

// true where the adapter reads asset class from the venue's catalogue; elsewhere it defaults to
// crypto
[[nodiscard]] bool states_asset_class(VenueId venue) noexcept;

// curated table first, then the adapter's verdict, then the default; a dex-prefixed name never
// defaults
void classify(VenueId venue, InstrumentBase& base) noexcept;

inline void classify(VenueId venue, Instrument& instrument) noexcept {
    std::visit([venue](auto& i) { classify(venue, i.base); }, instrument);
}

} // namespace raikiri
