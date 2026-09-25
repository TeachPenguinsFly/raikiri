#include <raikiri/config/settings.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <glaze/glaze.hpp>
#include <glaze/yaml.hpp>
#include <set>

namespace raikiri::config {
namespace {

template <typename T> std::expected<T, std::string> read_file(std::string_view path) {
    T out{};
    if (const auto ec = glz::read_file_yaml(out, path); ec) {
        return std::unexpected{std::string{path} + ": " + glz::format_error(ec)};
    }
    return out;
}

[[nodiscard]] bool windowed(std::string_view kind) noexcept {
    return kind == "rest" || kind == "ws_messages" || kind == "ws_connects";
}

[[nodiscard]] std::string check_limit(const RateLimitSettings& limit) {
    static constexpr std::array k_kinds{std::string_view{"rest"},
                                        std::string_view{"ws_messages"},
                                        std::string_view{"ws_connects"},
                                        std::string_view{"ws_streams"},
                                        std::string_view{"ws_sockets"},
                                        std::string_view{"ws_channels"},
                                        std::string_view{"ws_frame"}};
    if (limit.name.empty()) {
        return "a limit has no name";
    }
    if (std::ranges::find(k_kinds, std::string_view{limit.kind}) == k_kinds.end()) {
        return "unknown kind '" + limit.kind + "'";
    }
    if (limit.max == 0) {
        return "max is 0";
    }
    if (windowed(limit.kind) != (limit.window_ms != 0)) {
        return windowed(limit.kind) ? "window_ms is 0" : "window_ms on a count, not a rate";
    }
    if (limit.reserve >= limit.max && limit.reserve != 0) {
        return "reserve is not below max";
    }
    if (limit.kind == "ws_messages" && limit.scope != "socket" && limit.scope != "ip") {
        return "scope must be socket or ip";
    }
    if (limit.kind != "ws_messages" && !limit.scope.empty()) {
        return "scope applies only to ws_messages";
    }
    if (!limit.market.empty() && limit.market != "perp" && limit.market != "spot") {
        return "market must be perp or spot";
    }
    if (limit.kind != "rest" && (!limit.match.empty() || limit.reserve != 0)) {
        return "match and reserve apply only to rest";
    }
    if (std::ranges::any_of(limit.match, [](const std::string& m) { return m.empty(); })) {
        return "an empty match";
    }
    return {};
}

[[nodiscard]] std::expected<void, std::string> check(const VenuesFile& file) {
    for (const auto& [venue, settings] : file.venues) {
        std::set<std::string> names;
        for (const auto& limit : settings.limits) {
            if (auto why = check_limit(limit); !why.empty()) {
                return std::unexpected{venue + " limit '" + limit.name + "': " + why};
            }
            if (!names.insert(limit.name).second) {
                return std::unexpected{venue + " limit '" + limit.name + "': named twice"};
            }
        }
        for (const auto& cost : settings.costs) {
            if (cost.match.empty()) {
                return std::unexpected{venue + ": a cost rule with an empty match"};
            }
        }
    }
    return {};
}

} // namespace

std::expected<VenuesFile, std::string> load_venues(std::string_view path) {
    auto file = read_file<VenuesFile>(path);
    if (!file.has_value()) {
        return file;
    }
    if (const auto ok = check(*file); !ok.has_value()) {
        return std::unexpected{std::string{path} + ": " + ok.error()};
    }
    return file;
}

const RateLimitSettings*
find_limit(const VenueSettings& venue, std::string_view kind, std::string_view scope) noexcept {
    for (const auto& limit : venue.limits) {
        if (limit.kind == kind && (scope.empty() || limit.scope == scope)) {
            return &limit;
        }
    }
    return nullptr;
}

std::expected<DatabaseFile, std::string> load_database(std::string_view path) {
    return read_file<DatabaseFile>(path);
}

std::string config_dir() {
    if (const char* dir = std::getenv("RAIKIRI_CONFIG_DIR"); dir != nullptr && *dir != '\0') {
        return dir;
    }
    return "config";
}

} // namespace raikiri::config
