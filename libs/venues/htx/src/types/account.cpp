#include <raikiri/venues/htx/account.hpp>
#include <raikiri/venues/htx/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::htx {

namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

} // namespace

std::expected<std::uint64_t, WireError> parse_spot_account_id(std::string_view body) {
    return guarded([&]() -> std::expected<std::uint64_t, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = detail::req_array(*root, "data");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        for (auto entry : *array) {
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            const auto type = detail::opt_str(obj, "type");
            const auto state = detail::opt_str(obj, "state");
            const auto id = detail::opt_u64(obj, "id");
            if (!type.has_value() || !state.has_value() || !id.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            // An account carries margin, otc and point rows beside the spot
            // one; placing an order against the wrong id is answered
            // "invalid-parameter" rather than routed.
            if (*type == "spot" && *state == "working" && *id != 0) {
                return *id;
            }
        }
        return std::unexpected(WireError::MissingField);
    });
}

} // namespace raikiri::venues::htx
