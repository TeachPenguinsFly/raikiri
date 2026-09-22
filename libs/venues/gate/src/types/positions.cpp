#include <raikiri/venues/gate/envelope.hpp>
#include <raikiri/venues/gate/positions.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::gate {

namespace {

using wire::guarded;
using wire::session;

std::expected<void, WireError> read_position(wire::ondemand::object& obj, WirePosition& out) {
    const auto contract = wire::req_str(obj, "contract");
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
