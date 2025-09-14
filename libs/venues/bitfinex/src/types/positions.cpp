#include <raikiri/venues/bitfinex/envelope.hpp>
#include <raikiri/venues/bitfinex/positions.hpp>

namespace raikiri::venues::bitfinex {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;

std::expected<WirePosition, WireError> read_position(detail::ondemand::value value) {
    const auto row = read_row<k_position_arity>(value);
    if (!row.has_value()) {
        return std::unexpected(row.error());
    }
    WirePosition out{};
    out.symbol = row->template at<k_position_symbol>();
    out.status = row->template at<k_position_status>();
    out.amount = row->template at<k_position_amount>();
    out.base_price = row->template at<k_position_base_price>();
    out.mts_update = detail::digits_to_u64(row->template at<k_position_mts_update>());
    if (out.symbol.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return out;
}

} // namespace

std::expected<WirePosition, WireError> parse_position(std::string_view frame) {
    return guarded([&]() -> std::expected<WirePosition, WireError> {
        return with_row(frame,
                        [](detail::ondemand::value payload) { return read_position(payload); });
    });
}

std::expected<std::span<const WirePosition>, WireError> parse_positions(std::string_view body,
                                                                        PositionBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WirePosition>, WireError> {
        const auto count =
            rows_of<WirePosition>(body, storage.positions, [](detail::ondemand::value value) {
                return read_position(value);
            });
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        return std::span<const WirePosition>{storage.positions.data(), *count};
    });
}

} // namespace raikiri::venues::bitfinex
