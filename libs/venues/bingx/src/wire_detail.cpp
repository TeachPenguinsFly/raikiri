#include "wire_detail.hpp"

namespace raikiri::venues::bingx::detail {

std::expected<std::int64_t, WireError> req_i64(ondemand::object& obj, std::string_view key) {
    std::int64_t out = 0;
    if (const auto ec = obj.find_field_unordered(key).get_int64().get(out);
        ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return out;
}

std::string_view symbol_of(std::string_view data_type) {
    const auto at = data_type.find('@');
    return at == std::string_view::npos ? std::string_view{} : data_type.substr(0, at);
}

} // namespace raikiri::venues::bingx::detail
