#include <raikiri/venues/kraken/envelope.hpp>
#include <raikiri/venues/kraken/positions.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::kraken {

namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

} // namespace

std::expected<std::span<const WirePosition>, WireError> parse_positions(std::string_view body,
                                                                        PositionBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WirePosition>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = detail::req_array(*root, "openPositions");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_positions) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WirePosition out{};
            const auto symbol = detail::req_str(obj, "symbol");
            const auto side = detail::req_str(obj, "side");
            if (!symbol.has_value() || !side.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.symbol = *symbol;
            out.is_long = *side == "long";
            const auto price = req_num(obj, "price");
            const auto size = req_num(obj, "size");
            if (!price.has_value() || !size.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.price = *price;
            out.size = *size;
            storage.positions[count] = out;
            ++count;
        }
        return std::span<const WirePosition>{storage.positions.data(), count};
    });
}

} // namespace raikiri::venues::kraken
