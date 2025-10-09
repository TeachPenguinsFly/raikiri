#include <raikiri/venues/bingx/account.hpp>
#include <raikiri/venues/bingx/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::bingx {

namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

} // namespace

std::expected<std::string_view, WireError> parse_listen_key(std::string_view body) {
    return guarded([&]() -> std::expected<std::string_view, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto direct = detail::opt_str(*root, "listenKey");
        if (!direct.has_value()) {
            return std::unexpected(direct.error());
        }
        if (!direct->empty()) {
            return *direct;
        }
        auto data = detail::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        const auto nested = detail::req_str(*data, "listenKey");
        if (!nested.has_value()) {
            return std::unexpected(nested.error());
        }
        return *nested;
    });
}

std::expected<bool, WireError> parse_position_mode(std::string_view body) {
    return guarded([&]() -> std::expected<bool, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = detail::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        const auto text = req_scalar(*data, "dualSidePosition");
        if (!text.has_value()) {
            return std::unexpected(text.error());
        }
        return *text == "true";
    });
}

} // namespace raikiri::venues::bingx
