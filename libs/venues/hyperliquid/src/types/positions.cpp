#include <raikiri/venues/hyperliquid/envelope.hpp>
#include <raikiri/venues/hyperliquid/positions.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::hyperliquid {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
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
        auto rows = detail::req_array(*root, "assetPositions");
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        std::size_t count = 0;
        for (auto row : *rows) {
            if (count == k_max_positions) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object wrapper;
            if (const auto ec = row.get_object().get(wrapper); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            // The `type` beside it ("oneWay") is the venue's position mode and
            // the domain has no other; reading it would only invite a switch.
            auto position = detail::req_object(wrapper, "position");
            if (!position.has_value()) {
                return std::unexpected(position.error());
            }
            const auto coin = detail::req_str(*position, "coin");
            const auto size = detail::req_str(*position, "szi");
            const auto entry = detail::req_str(*position, "entryPx");
            if (!coin.has_value() || !size.has_value() || !entry.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            storage.positions[count] = {.coin = *coin, .size = *size, .entry_px = *entry};
            ++count;
        }
        // After the array, never before: On-Demand cannot revisit a key it has
        // already walked past, and `time` trails assetPositions in the body.
        const auto ts = opt_number_ms(*root, "time");
        if (!ts.has_value()) {
            return std::unexpected(ts.error());
        }
        storage.ts_ms = *ts;
        return std::span<const WirePosition>{storage.positions.data(), count};
    });
}

} // namespace raikiri::venues::hyperliquid
