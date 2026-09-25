#include <raikiri/venues/htx/envelope.hpp>
#include <raikiri/venues/htx/positions.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::htx {

namespace {

using wire::guarded;
using wire::session;

} // namespace

std::expected<std::span<const WirePosition>, WireError> parse_positions(std::string_view body,
                                                                        PositionBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WirePosition>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = wire::req_array(*root, "data");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        const auto ts = wire::opt_u64(*root, "ts");
        if (!ts.has_value()) {
            return std::unexpected(ts.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_positions) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            const auto code = wire::req_str(obj, "contract_code");
            const auto volume = num_of(obj, "volume");
            // cost_hold is the entry; cost_open stops moving after a partial close
            const auto cost = num_of(obj, "cost_hold");
            const auto direction = wire::opt_str(obj, "direction");
            if (!code.has_value() || !volume.has_value() || !cost.has_value() || !direction.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            storage.positions[count] = {.contract_code = *code,
                                        .volume = *volume,
                                        .cost_hold = *cost,
                                        .sell = *direction == "sell",
                                        .ts_ms = *ts};
            ++count;
        }
        return std::span<const WirePosition>{storage.positions.data(), count};
    });
}

} // namespace raikiri::venues::htx
