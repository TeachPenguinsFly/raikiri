#pragma once

// What the venue lists, and the numbers that decide whether an order it
// accepts is an order it will fill.

#include <raikiri/venues/bitmex/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bitmex {

// A row of GET /instrument/active, whose name is a lie: it returns Unlisted and
// Settled rows too (16 of 66 on capture day), so `state` decides, not the URL.
// The three scale fields are mutually exclusive by contract type, and which one
// is present is how BitMEX says inverse, linear or spot. map.cpp reads them.
struct WireInstrument {
    std::string_view symbol;
    std::string_view state; // "Open" / "Unlisted" / "Settled"
    std::string_view typ;   // FFWCSX perp, FFSCSX perp, FFCCSX dated, IFXXXP spot
    std::string_view tick_size;
    std::string_view lot_size;
    std::string_view multiplier;
    std::string_view underlying_to_settle;   // inverse rows
    std::string_view quote_to_settle;        // linear rows
    std::string_view underlying_to_position; // spot rows
    std::string_view expiry;                 // dated rows
    std::string_view funding_interval;
    std::string_view maker_fee;
    std::string_view taker_fee;
    bool inverse{false};
    bool quanto{false};
};

inline constexpr std::size_t k_max_instruments = 512;

struct InstrumentBuffer {
    std::array<WireInstrument, k_max_instruments> instruments{};
};

// The body is a bare JSON array; there is no envelope to unwrap first.
[[nodiscard]] std::expected<std::span<const WireInstrument>, WireError>
parse_instruments(std::string_view body, InstrumentBuffer& storage);

} // namespace raikiri::venues::bitmex
