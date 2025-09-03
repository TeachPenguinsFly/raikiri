#include <raikiri/config/settings.hpp>

#include <cstdlib>
#include <glaze/glaze.hpp>
#include <glaze/yaml.hpp>

namespace raikiri::config {
namespace {

template <typename T> std::expected<T, std::string> read_file(std::string_view path) {
    T out{};
    if (const auto ec = glz::read_file_yaml(out, path); ec) {
        return std::unexpected{std::string{path} + ": " + glz::format_error(ec)};
    }
    return out;
}

} // namespace

std::expected<VenuesFile, std::string> load_venues(std::string_view path) {
    return read_file<VenuesFile>(path);
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
