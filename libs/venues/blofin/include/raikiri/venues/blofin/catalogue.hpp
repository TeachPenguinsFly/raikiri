#pragma once

#include <raikiri/venues/blofin/errors.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::blofin {

struct WireInstrument {
    std::string_view inst_id;
    std::string_view inst_type; // SWAP
    std::string_view state;     // live
    std::string_view base_ccy;
    std::string_view quote_ccy;
    std::string_view settle_ccy;
    std::string_view contract_type; // linear | inverse
    // Base units on a linear contract; on an inverse one it is USD, whatever the docs say.
    std::string_view contract_value;
    std::string_view tick_size;
    std::string_view lot_size; // in contracts
};

inline constexpr std::size_t k_max_instruments = 2048;

struct InstrumentBuffer {
    std::array<WireInstrument, k_max_instruments> rows{};
};

[[nodiscard]] std::expected<std::span<const WireInstrument>, WireError>
parse_instruments(std::string_view body, InstrumentBuffer& storage);

} // namespace raikiri::venues::blofin
