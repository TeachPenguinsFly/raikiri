#include <raikiri/venues/okx/envelope.hpp>
#include <raikiri/venues/okx/positions.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::okx {

namespace {

using wire::guarded;

std::expected<void, WireError> read_position(wire::ondemand::object& obj, WirePosition& out) {
    const auto inst_id = wire::req_str(obj, "instId");
    if (!inst_id.has_value()) {
        return std::unexpected(inst_id.error());
    }
    out.inst_id = *inst_id;
    if (const auto ok = read_strings(obj, {{"pos", &out.pos}, {"posSide", &out.pos_side}, {"avgPx", &out.avg_px}});
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

std::expected<std::uint32_t, WireError> parse_leverage_info(std::string_view body) {
    return wire::guarded([&]() -> std::expected<std::uint32_t, WireError> {
        auto root = wire::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = wire::req_array(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        for (auto entry : *data) {
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            const auto lever = wire::req_str(obj, "lever");
            if (!lever.has_value()) {
                return std::unexpected(lever.error());
            }
            return static_cast<std::uint32_t>(wire::digits_to_u64(lever->substr(0, lever->find('.'))));
        }
        return std::unexpected(wire::map_error(simdjson::NO_SUCH_FIELD));
    });
}

} // namespace raikiri::venues::okx
