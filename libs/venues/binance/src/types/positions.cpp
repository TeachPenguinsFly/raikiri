#include <raikiri/venues/binance/envelope.hpp>
#include <raikiri/venues/binance/positions.hpp>
#include <raikiri/wire/json.hpp>

#include <array>

namespace raikiri::venues::binance {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

} // namespace

std::expected<std::span<const WirePosition>, WireError>
parse_account_update(std::string_view data, PositionBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WirePosition>, WireError> {
        auto root = session().open_object(data);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto account = detail::req_object(*root, "a");
        if (!account.has_value()) {
            return std::unexpected(account.error());
        }
        auto positions = detail::req_array(*account, "P");
        if (!positions.has_value()) {
            return std::unexpected(positions.error());
        }
        std::size_t count = 0;
        for (auto entry : *positions) {
            if (count == k_max_positions) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WirePosition out{};
            const std::array<Wanted, 4> wanted{{{"s", &out.symbol},
                                                {"pa", &out.position_amt},
                                                {"ep", &out.entry_price},
                                                {"ps", &out.position_side}}};
            if (const auto ok = read_strings(obj, wanted); !ok.has_value()) {
                return std::unexpected(ok.error());
            }
            storage.positions[count] = out;
            ++count;
        }
        return std::span<const WirePosition>{storage.positions.data(), count};
    });
}

std::expected<std::span<const WirePositionRisk>, WireError>
parse_position_risk(std::string_view body, std::span<WirePositionRisk> storage) {
    return guarded([&]() -> std::expected<std::span<const WirePositionRisk>, WireError> {
        auto array = session().open_array(body);
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == storage.size()) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WirePositionRisk out{};
            const std::array<Wanted, 3> wanted{{{"symbol", &out.symbol},
                                                {"positionAmt", &out.position_amt},
                                                {"entryPrice", &out.entry_price}}};
            if (const auto ok = read_strings(obj, wanted); !ok.has_value()) {
                return std::unexpected(ok.error());
            }
            storage[count] = out;
            ++count;
        }
        return std::span<const WirePositionRisk>{storage.data(), count};
    });
}

} // namespace raikiri::venues::binance
