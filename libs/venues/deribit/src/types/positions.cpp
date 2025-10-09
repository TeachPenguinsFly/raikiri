#include <raikiri/venues/deribit/envelope.hpp>
#include <raikiri/venues/deribit/positions.hpp>
#include <raikiri/wire/json.hpp>

#include <utility>

namespace raikiri::venues::deribit {

namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

std::expected<WirePosition, WireError> read_position(detail::ondemand::object& obj) {
    WirePosition out{};
    const auto instrument = detail::opt_str(obj, "instrument_name");
    if (!instrument.has_value()) {
        return std::unexpected(instrument.error());
    }
    if (instrument->empty()) {
        return std::unexpected(WireError::MissingField);
    }
    out.instrument = *instrument;
    for (const auto& [key, field] :
         {std::pair{"size", &out.size}, std::pair{"average_price", &out.average_price}}) {
        const auto value = opt_num(obj, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *field = *value;
    }
    return out;
}

} // namespace

std::expected<std::span<const WirePosition>, WireError> parse_positions(std::string_view body,
                                                                        PositionBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WirePosition>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto count = read_rows<WirePosition>(
            *root, "result", storage.positions, [](detail::ondemand::object& obj) {
                return read_position(obj);
            });
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        return std::span<const WirePosition>{storage.positions.data(), *count};
    });
}

} // namespace raikiri::venues::deribit
