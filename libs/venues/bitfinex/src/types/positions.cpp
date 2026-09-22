#include <raikiri/venues/bitfinex/envelope.hpp>
#include <raikiri/venues/bitfinex/positions.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::bitfinex {

namespace {

using wire::guarded;

std::expected<WirePosition, WireError> read_position(wire::ondemand::value value) {
    const auto row = read_row<k_position_arity>(value);
    if (!row.has_value()) {
        return std::unexpected(row.error());
    }
    WirePosition out{};
    out.symbol = row->template at<k_position_symbol>();
    out.status = row->template at<k_position_status>();
    out.amount = row->template at<k_position_amount>();
    out.base_price = row->template at<k_position_base_price>();
    out.mts_update = wire::digits_to_u64(row->template at<k_position_mts_update>());
    if (out.symbol.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return out;
}

} // namespace

std::expected<WirePosition, WireError> parse_position(std::string_view frame) {
    return guarded([&]() -> std::expected<WirePosition, WireError> {
        return with_row(frame,
                        [](wire::ondemand::value payload) { return read_position(payload); });
    });
}

std::expected<std::span<const WirePosition>, WireError> parse_positions(std::string_view body,
                                                                        PositionBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WirePosition>, WireError> {
        const auto count =
            rows_of<WirePosition>(body, storage.positions, [](wire::ondemand::value value) {
                return read_position(value);
            });
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        return std::span<const WirePosition>{storage.positions.data(), *count};
    });
}

} // namespace raikiri::venues::bitfinex
