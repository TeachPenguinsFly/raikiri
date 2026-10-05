#pragma once

#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/instrument.hpp>

#include <span>
#include <string>
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
// defaults. an underlying the adapter already set wins over the local symbol's base
void classify(VenueId venue, InstrumentBase& base) noexcept;

// what cross-venue matching keys on: "equity:TSLA", "commodity:XAU"; empty when unclassified
[[nodiscard]] std::string asset_key(const InstrumentBase& base);

// the asset key and the quote with usdt and usdc folded together: "equity:TSLA/USD*"
[[nodiscard]] std::string market_key(const InstrumentBase& base);

// the hip-3 deployer a listing comes from, "xyz" of "xyz:TSLA/USDC"; empty elsewhere
[[nodiscard]] std::string_view dex_of(const InstrumentBase& base) noexcept;

inline void classify(VenueId venue, Instrument& instrument) noexcept {
    std::visit([venue](auto& i) { classify(venue, i.base); }, instrument);
}

} // namespace raikiri
