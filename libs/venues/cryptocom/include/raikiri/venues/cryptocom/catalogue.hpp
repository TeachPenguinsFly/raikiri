#pragma once

#include <raikiri/venues/cryptocom/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::cryptocom {

struct WireInstrument {
    std::string_view symbol;
    std::string_view inst_type; // CCY_PAIR, PERPETUAL_SWAP, FUTURE
    std::string_view base_ccy;
    std::string_view quote_ccy;
    std::string_view price_tick_size;
    std::string_view qty_tick_size;
    std::string_view contract_size; // absent on a pair
    bool tradable{false};
};

inline constexpr std::size_t k_max_instruments = 2048;

struct InstrumentBuffer {
    std::array<WireInstrument, k_max_instruments> rows{};
};

[[nodiscard]] std::expected<std::span<const WireInstrument>, WireError>
parse_instruments(std::string_view body, InstrumentBuffer& storage);

} // namespace raikiri::venues::cryptocom
