#pragma once

// What the venue lists, and the numbers that decide whether an order it
// accepts is an order it will fill.
//
// WEEX states no tick size and no lot size by name. It states PRECISIONS, and
// the two have to be read differently:
//
//   the price step is 10^-pricePrecision and has to be computed. Live values
//   run 1 through 10, and 10 is a step of 1e-10 — finer than FixedE9 holds, so
//   those two rows are refused rather than rounded into something tradable.
//
//   the size step is `contractVal`, which WEEX writes out as a decimal. It
//   equalled 10^-quantityPrecision on 1022 of 1022 rows, so reading the decimal
//   costs nothing and sidesteps the trap in the other field: quantityPrecision
//   is NEGATIVE on 385 of them (down to -6), meaning a size that steps in
//   millions, and an exponent computed as if it were positive would put the
//   step six orders of magnitude out.
//
// There is NO status field on a row. Every listed contract is tradable, which
// is why nothing here filters on one — and why a reader looking for the usual
// TRADING check should stop looking rather than invent one.
//
// There is also NO minimum notional. `minOrderSize` is a base-asset QUANTITY
// (it equals the size step on 1020 of 1022 rows), and filing a quantity in a
// notional field would have the order gate comparing a coin count against a
// dollar figure. The instrument's min_notional stays UNDEF: WEEX did not say.

#include <raikiri/venues/weex/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::weex {

struct WireInstrument {
    std::string_view symbol;
    std::string_view base_asset;
    std::string_view quote_asset;
    // PERPETUAL or TRADIFI_PERPETUAL. The second is a perpetual on a non-crypto
    // underlying — 446 of them live, quoting FX, metals and equity indices —
    // and it settles, funds and margins in USDT exactly like the first.
    std::string_view contract_type;
    // A decimal, and the size step. JSON writes it as an integer on 573 rows
    // and as a float on 449, in one body.
    std::string_view contract_val;
    std::uint64_t price_precision{0};
    // How many times a day the contract settles funding, read as the LENGTH of
    // the `delivery` timetable. Its entries are clock times ("00:00:00",
    // "08:00:00", ...) and one row spells one of them with a leading space, so
    // only the count is read. The count agreed with premiumIndex's own
    // collectCycle on all 1022 rows: 3 -> 480 min, 6 -> 240, 24 -> 60.
    std::uint32_t settlements_per_day{0};
};

// 1022 contracts live on 2026-09-07. The cap is the buffer's, not the venue's.
inline constexpr std::size_t k_max_instruments = 2048;

struct InstrumentBuffer {
    std::array<WireInstrument, k_max_instruments> instruments{};
};

[[nodiscard]] std::expected<std::span<const WireInstrument>, WireError>
parse_exchange_info(std::string_view body, InstrumentBuffer& storage);

} // namespace raikiri::venues::weex
