#include "wire_detail.hpp"

namespace raikiri::venues::bitmex::detail {

std::expected<std::string_view, WireError> opt_num(ondemand::object& obj, std::string_view key) {
    const auto raw = opt_raw(obj, key);
    if (!raw.has_value()) {
        return std::unexpected(raw.error());
    }
    if (raw->empty() || *raw == "null") {
        return std::string_view{};
    }
    if (raw->front() == '"' || raw->front() == '{' || raw->front() == '[') {
        return std::unexpected(WireError::WrongType);
    }
    return *raw;
}

} // namespace raikiri::venues::bitmex::detail
