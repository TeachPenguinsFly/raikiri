#include <raikiri/venues/gate/envelope.hpp>
#include <raikiri/venues/gate/positions.hpp>

namespace raikiri::venues::gate {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson layer;
// a venue that reimplements opt_str or req_array is one that will disagree
// with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

std::expected<void, WireError> read_position(detail::ondemand::object& obj, WirePosition& out) {
    const auto contract = detail::req_str(obj, "contract");
    if (!contract.has_value()) {
        return std::unexpected{contract.error()};
    }
    out.contract = *contract;
    out.size = field(obj, "size");
    out.entry_price = field(obj, "entry_price");
    out.update_s = whole_u64(field(obj, "update_time"));
    return {};
}

} // namespace

std::expected<std::span<const WirePosition>, WireError> parse_positions(std::string_view body,
                                                                        PositionBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WirePosition>, WireError> {
        auto root = session().open_array(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return read_array<WirePosition>(*root, storage.positions, read_position);
    });
}

} // namespace raikiri::venues::gate
