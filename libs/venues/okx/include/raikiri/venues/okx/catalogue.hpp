#pragma once

#include <raikiri/common/time.hpp>
#include <raikiri/venues/okx/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::okx {

// ct_val/ct_mult are empty on SPOT rows and on rows that are not live yet.
struct WireInstrument {
    std::string_view inst_id;
    std::string_view inst_type; // SWAP, FUTURES, SPOT, OPTION
    std::string_view state;     // live, preopen, suspend, expired
    std::string_view base_ccy;  // SPOT only; empty on SWAP, FUTURES and OPTION
    std::string_view quote_ccy; // SPOT only; empty on SWAP, FUTURES and OPTION
    std::string_view tick_size;
    std::string_view lot_size;
    std::string_view min_size;
    std::string_view ct_val;
    std::string_view ct_mult;
    std::string_view ct_type;       // linear | inverse; empty on SPOT and OPTION rows
    std::string_view inst_category; // 1 crypto, 3 stocks, 4 commodities, 5 forex
    std::uint64_t inst_id_code{0};  // zero while okx has not issued one; order ops on the socket need it
    std::string_view inst_family;   // BTC-USD, BTC-USD_UM; empty on SPOT
    std::string_view uly;
    std::string_view strike;   // OPTION only, in usd
    std::string_view opt_type; // C | P
    std::uint64_t expiry_ms{0};
};

// the spot catalogue is about three times the swap one
inline constexpr std::size_t k_max_instruments = 4096;

struct InstrumentBuffer {
    std::array<WireInstrument, k_max_instruments> instruments{};
};

// GET /public/underlying answers one array of names inside data
inline constexpr std::size_t k_max_underlyings = 64;

struct UnderlyingBuffer {
    std::array<std::string_view, k_max_underlyings> names{};
};

[[nodiscard]] std::expected<std::span<const std::string_view>, WireError> parse_underlyings(std::string_view body,
                                                                                            UnderlyingBuffer& storage);

[[nodiscard]] std::expected<std::span<const WireInstrument>, WireError> parse_instruments(std::string_view body,
                                                                                          InstrumentBuffer& storage);

} // namespace raikiri::venues::okx
