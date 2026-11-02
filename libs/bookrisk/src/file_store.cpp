#include <raikiri/bookrisk/file_store.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <glaze/glaze.hpp>
#include <map>
#include <sstream>
#include <string_view>

namespace raikiri::bookrisk {

constexpr glz::opts k_write{.skip_null_members = true, .prettify = true};
constexpr glz::opts k_read{.error_on_unknown_keys = false};

struct LimitDoc {
    std::string mode{"inherit"};
    std::optional<std::string> value{};
};

struct RowDoc {
    std::map<std::string, LimitDoc> limits{};
    std::optional<std::string> daily_loss_action{};
};

struct StateDoc {
    std::string state{"active"};
    std::string reason{};
    std::string by{};
    std::string since{"0"};
};

struct DayDoc {
    std::uint32_t day{0};
    std::string start_pnl{"0"};
};

struct BookDoc {
    RowDoc limits{};
    StateDoc state{};
    DayDoc day{};
};

struct SettingsDoc {
    std::uint32_t version{1};
    std::optional<std::string> capital{};
    std::string capital_by{};
    std::string capital_at{"0"};
    RowDoc defaults{};
    StateDoc desk{};
    std::map<std::string, BookDoc> books{};
};

namespace internal {

[[nodiscard]] std::string text_of(FixedE9 value) {
    std::array<char, 32> buf{};
    return std::string{format_e9(value, buf.data(), buf.size())};
}

[[nodiscard]] RowDoc to_doc(const LimitRow& row) {
    RowDoc out{};
    for (const auto& info : k_params) {
        const auto& limit = row[info.param];
        if (limit.mode == LimitMode::Inherit) {
            continue;
        }
        out.limits[std::string{info.name}] = {.mode = std::string{to_string(limit.mode)},
                                              .value = limit.value.is_undef() ? std::optional<std::string>{}
                                                                              : std::optional{text_of(limit.value)}};
    }
    if (row.loss_action.has_value()) {
        out.daily_loss_action = std::string{to_string(*row.loss_action)};
    }
    return out;
}

[[nodiscard]] StateDoc to_doc(const StateRecord& state) {
    return {.state = std::string{oms::to_string(state.state)},
            .reason = state.reason,
            .by = state.by,
            .since = std::to_string(state.since.ns)};
}

[[nodiscard]] std::expected<LimitRow, std::string> from_doc(const RowDoc& doc) {
    LimitRow out{};
    for (const auto& [name, limit] : doc.limits) {
        const auto param = param_from(name);
        const auto mode = limit_mode_from(limit.mode);
        if (!param.has_value() || !mode.has_value()) {
            return std::unexpected{"unknown limit " + name + " or mode " + limit.mode};
        }
        auto value = FixedE9::undef();
        if (limit.value.has_value()) {
            const auto parsed = parse_decimal_e9(*limit.value);
            if (!parsed.has_value()) {
                return std::unexpected{"limit " + name + " is not a decimal"};
            }
            value = *parsed;
        }
        out[*param] = {.mode = *mode, .value = value};
    }
    if (doc.daily_loss_action.has_value()) {
        out.loss_action = loss_action_from(*doc.daily_loss_action);
        if (!out.loss_action.has_value()) {
            return std::unexpected{"unknown daily_loss_action " + *doc.daily_loss_action};
        }
    }
    return out;
}

[[nodiscard]] std::expected<StateRecord, std::string> from_doc(const StateDoc& doc) {
    const auto state = oms::book_state_from(doc.state);
    if (!state.has_value()) {
        return std::unexpected{"unknown state " + doc.state};
    }
    return StateRecord{
        .state = *state, .reason = doc.reason, .by = doc.by, .since = {std::strtoull(doc.since.c_str(), nullptr, 10)}};
}

[[nodiscard]] std::string os_error(std::string_view what, const std::string& path) {
    return std::string{what} + " " + path + ": " + std::strerror(errno);
}

} // namespace internal

std::expected<std::optional<DeskSettings>, std::string> FileSettingsStore::load() {
    std::error_code absent;
    if (!std::filesystem::exists(path_, absent)) {
        return std::optional<DeskSettings>{};
    }
    std::ifstream in{path_};
    if (!in) {
        return std::unexpected{internal::os_error("cannot read", path_)};
    }
    std::stringstream text;
    text << in.rdbuf();
    SettingsDoc doc{};
    if (const auto ec = glz::read<k_read>(doc, text.str()); ec) {
        return std::unexpected{path_ + ": " + glz::format_error(ec, text.str())};
    }
    DeskSettings out{};
    if (doc.capital.has_value()) {
        const auto parsed = parse_decimal_e9(*doc.capital);
        if (!parsed.has_value()) {
            return std::unexpected{path_ + ": capital is not a decimal"};
        }
        out.capital = *parsed;
    }
    out.capital_by = doc.capital_by;
    out.capital_at = {std::strtoull(doc.capital_at.c_str(), nullptr, 10)};
    auto defaults = internal::from_doc(doc.defaults);
    auto desk = internal::from_doc(doc.desk);
    if (!defaults.has_value() || !desk.has_value()) {
        return std::unexpected{path_ + ": " + (!defaults.has_value() ? defaults.error() : desk.error())};
    }
    out.defaults = *defaults;
    out.desk = *desk;
    for (const auto& [key, book] : doc.books) {
        const auto id = std::strtoul(key.c_str(), nullptr, 10);
        auto limits = internal::from_doc(book.limits);
        auto state = internal::from_doc(book.state);
        const auto start = parse_decimal_e9(book.day.start_pnl);
        if (id > 0xFFFF || !limits.has_value() || !state.has_value() || !start.has_value()) {
            return std::unexpected{path_ + ": book " + key + " cannot be read"};
        }
        out.books[static_cast<BookId>(id)] = {
            .limits = *limits, .state = *state, .day = {.day = book.day.day, .start_pnl = *start}};
    }
    return std::optional<DeskSettings>{std::move(out)};
}

std::expected<void, std::string> FileSettingsStore::save(const DeskSettings& settings) {
    SettingsDoc doc{.capital = settings.capital.is_undef() ? std::optional<std::string>{}
                                                           : std::optional{internal::text_of(settings.capital)},
                    .capital_by = settings.capital_by,
                    .capital_at = std::to_string(settings.capital_at.ns),
                    .defaults = internal::to_doc(settings.defaults),
                    .desk = internal::to_doc(settings.desk)};
    for (const auto& [id, book] : settings.books) {
        const bool empty = book.limits == LimitRow{} && book.state.state == oms::BookState::Active &&
                           book.state.by.empty() && book.day.day == 0;
        if (empty) {
            continue;
        }
        doc.books[std::to_string(id)] = {
            .limits = internal::to_doc(book.limits),
            .state = internal::to_doc(book.state),
            .day = {.day = book.day.day, .start_pnl = internal::text_of(book.day.start_pnl)}};
    }
    std::string body;
    if (const auto ec = glz::write<k_write>(doc, body); ec) {
        return std::unexpected{"cannot serialise the risk settings"};
    }
    const std::filesystem::path target{path_};
    std::error_code made;
    std::filesystem::create_directories(target.parent_path(), made);
    const auto temp = path_ + ".tmp";
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return std::unexpected{internal::os_error("cannot open", temp)};
    }
    std::string_view left{body};
    while (!left.empty()) {
        const auto wrote = ::write(fd, left.data(), left.size());
        if (wrote < 0) {
            const auto why = internal::os_error("cannot write", temp);
            ::close(fd);
            return std::unexpected{why};
        }
        left.remove_prefix(static_cast<std::size_t>(wrote));
    }
    const bool synced = ::fsync(fd) == 0;
    ::close(fd);
    if (!synced) {
        return std::unexpected{internal::os_error("cannot sync", temp)};
    }
    if (::rename(temp.c_str(), path_.c_str()) != 0) {
        return std::unexpected{internal::os_error("cannot replace", path_)};
    }
    const auto folder = target.parent_path().empty() ? std::string{"."} : target.parent_path().string();
    if (const int dir = ::open(folder.c_str(), O_RDONLY); dir >= 0) {
        (void)::fsync(dir);
        ::close(dir);
    }
    return {};
}

} // namespace raikiri::bookrisk
