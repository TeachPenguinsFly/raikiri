#include <raikiri/audit/file_store.hpp>
#include <raikiri/audit/line.hpp>
#include <raikiri/audit/verify.hpp>

#include <algorithm>
#include <filesystem>

namespace raikiri::audit {

std::vector<std::string> FileStore::streams() const {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator{root_, ec}) {
        if (item.is_directory(ec)) {
            out.push_back(item.path().filename().string());
        }
    }
    std::ranges::sort(out);
    return out;
}

std::vector<std::uint32_t> FileStore::days(const std::string& stream) const {
    return list_days(root_ + "/" + stream);
}

void FileStore::scan_day(const std::string& stream,
                         std::uint32_t day,
                         const std::function<void(const EntryView&)>& visit) const {
    const auto content = internal::read_file(day_path(root_ + "/" + stream, day));
    if (!content.has_value()) {
        return;
    }
    std::string actor;
    std::string action;
    std::string subject;
    std::string_view rest{*content};
    for (auto stop = rest.find('\n'); stop != std::string_view::npos; stop = rest.find('\n')) {
        const auto line = rest.substr(0, stop);
        rest.remove_prefix(stop + 1);
        const auto parsed = parse_line(line);
        if (!parsed.has_value()) {
            continue;
        }
        unescape_to(actor, parsed->actor);
        unescape_to(action, parsed->action);
        unescape_to(subject, parsed->subject);
        visit({.seq = parsed->seq,
               .ts = {parsed->ts},
               .actor = actor,
               .action = action,
               .subject = subject,
               .detail_json = parsed->detail,
               .prev = parsed->prev,
               .line = line});
    }
}

ChainReport FileStore::verify(const std::string& stream, std::uint32_t day) const {
    return verify_day(root_ + "/" + stream, day);
}

} // namespace raikiri::audit
