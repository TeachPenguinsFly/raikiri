#include <raikiri/internalcross/infrastructure/file_store.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <glaze/glaze.hpp>
#include <sstream>
#include <string_view>

namespace raikiri::internalcross {

namespace internal {

constexpr glz::opts k_write{.prettify = true};
constexpr glz::opts k_read{.error_on_unknown_keys = false};

struct BookDoc {
    bool enabled{false};
    std::int64_t fee_bp{0};
    std::vector<std::string> instruments{};
};

struct Doc {
    bool enabled{false};
    std::int64_t max_spread_bp{20};
    std::uint64_t max_quote_age_ms{1000};
    std::uint64_t hold_ms{0};
    bool against_desk{false};
    std::map<std::string, BookDoc> books{};
};

} // namespace internal

std::expected<std::optional<Settings>, std::string> FileSettingsStore::load() {
    std::ifstream in{path_};
    if (!in.is_open()) {
        return std::optional<Settings>{};
    }
    std::ostringstream body;
    body << in.rdbuf();
    internal::Doc doc{};
    if (const auto ec = glz::read<internal::k_read>(doc, body.str()); ec) {
        return std::unexpected{path_ + ": " + glz::format_error(ec, body.str())};
    }
    Settings out{.enabled = doc.enabled,
                 .max_spread_bp = doc.max_spread_bp,
                 .max_quote_age_ms = doc.max_quote_age_ms,
                 .hold_ms = doc.hold_ms,
                 .against_desk = doc.against_desk};
    for (auto& [name, book] : doc.books) {
        out.books[name] = {.enabled = book.enabled, .fee_bp = book.fee_bp, .instruments = std::move(book.instruments)};
    }
    return std::optional{std::move(out)};
}

std::expected<void, std::string> FileSettingsStore::save(const Settings& settings) {
    internal::Doc doc{.enabled = settings.enabled,
                      .max_spread_bp = settings.max_spread_bp,
                      .max_quote_age_ms = settings.max_quote_age_ms,
                      .hold_ms = settings.hold_ms,
                      .against_desk = settings.against_desk};
    for (const auto& [name, book] : settings.books) {
        doc.books[name] = {.enabled = book.enabled, .fee_bp = book.fee_bp, .instruments = book.instruments};
    }
    std::string body;
    if (const auto ec = glz::write<internal::k_write>(doc, body); ec) {
        return std::unexpected{"cannot serialise the crossing settings"};
    }
    const std::filesystem::path target{path_};
    std::error_code made;
    std::filesystem::create_directories(target.parent_path(), made);
    const auto temp = path_ + ".tmp";
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return std::unexpected{"cannot open " + temp};
    }
    std::string_view left{body};
    while (!left.empty()) {
        const auto wrote = ::write(fd, left.data(), left.size());
        if (wrote < 0) {
            ::close(fd);
            return std::unexpected{"cannot write " + temp};
        }
        left.remove_prefix(static_cast<std::size_t>(wrote));
    }
    const bool synced = ::fsync(fd) == 0;
    ::close(fd);
    if (!synced || ::rename(temp.c_str(), path_.c_str()) != 0) {
        return std::unexpected{"cannot replace " + path_};
    }
    return {};
}

} // namespace raikiri::internalcross
