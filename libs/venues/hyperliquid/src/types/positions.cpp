#include <raikiri/venues/hyperliquid/envelope.hpp>
#include <raikiri/venues/hyperliquid/positions.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::hyperliquid {

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
        auto rows = wire::req_array(*root, "assetPositions");
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        std::size_t count = 0;
        for (auto row : *rows) {
            if (count == k_max_positions) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object wrapper;
            if (const auto ec = row.get_object().get(wrapper); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            auto position = wire::req_object(wrapper, "position");
            if (!position.has_value()) {
                return std::unexpected(position.error());
            }
            const auto coin = wire::req_str(*position, "coin");
            const auto size = wire::req_str(*position, "szi");
            const auto entry = wire::req_str(*position, "entryPx");
            if (!coin.has_value() || !size.has_value() || !entry.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            storage.positions[count] = {.coin = *coin, .size = *size, .entry_px = *entry};
            ++count;
        }
        // `time` trails assetPositions, and on-demand cannot go back
        const auto ts = opt_number_ms(*root, "time");
        if (!ts.has_value()) {
            return std::unexpected(ts.error());
        }
        storage.ts_ms = *ts;
        return std::span<const WirePosition>{storage.positions.data(), count};
    });
}

} // namespace raikiri::venues::hyperliquid
