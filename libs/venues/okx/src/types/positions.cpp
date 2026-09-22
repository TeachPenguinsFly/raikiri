#include <raikiri/venues/okx/envelope.hpp>
#include <raikiri/venues/okx/positions.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::okx {

namespace {

using wire::guarded;
using wire::session;

std::expected<void, WireError> read_position(wire::ondemand::object& obj, WirePosition& out) {
    const auto inst_id = wire::req_str(obj, "instId");
    if (!inst_id.has_value()) {
        return std::unexpected(inst_id.error());
    }
    out.inst_id = *inst_id;
    if (const auto ok = read_strings(
            obj, {{"pos", &out.pos}, {"posSide", &out.pos_side}, {"avgPx", &out.avg_px}});
        !ok.has_value()) {
        return std::unexpected(ok.error());
    }
    const auto u_ms = wire::opt_ms(obj, "uTime");
    if (!u_ms.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.u_ms = *u_ms;
    return {};
}

} // namespace

std::expected<std::span<const WirePosition>, WireError> parse_positions(std::string_view body,
                                                                        PositionBuffer& storage) {
    return guarded([&] { return read_data<WirePosition>(body, storage.positions, read_position); });
}

} // namespace raikiri::venues::okx
