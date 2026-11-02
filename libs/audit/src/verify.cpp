#include <raikiri/audit/line.hpp>
#include <raikiri/audit/verify.hpp>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace raikiri::audit {

namespace internal {

constexpr std::string_view k_day_suffix = ".jsonl";

std::optional<std::string> read_file(const std::string& path) {
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    if (!file) {
        return std::nullopt;
    }
    std::string content(static_cast<std::size_t>(file.tellg()), '\0');
    file.seekg(0);
    file.read(content.data(), static_cast<std::streamsize>(content.size()));
    content.resize(static_cast<std::size_t>(file.gcount()));
    return content;
}

// from `at` to the end; past the end reads as empty
[[nodiscard]] std::optional<std::string> read_from(const std::string& path, std::uint64_t at) {
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    if (!file) {
        return std::nullopt;
    }
    const auto size = static_cast<std::uint64_t>(file.tellg());
    if (at >= size) {
        return std::string{};
    }
    std::string content(size - at, '\0');
    file.seekg(static_cast<std::streamoff>(at));
    file.read(content.data(), static_cast<std::streamsize>(content.size()));
    content.resize(static_cast<std::size_t>(file.gcount()));
    return content;
}

constexpr std::uint64_t k_tail_bytes = std::uint64_t{64} * 1024;

std::optional<Tip> tail_of(const std::string& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return std::nullopt;
    }
    const auto content = read_from(path, size > k_tail_bytes ? size - k_tail_bytes : 0);
    if (!content.has_value()) {
        return std::nullopt;
    }
    const auto end = content->rfind('\n');
    if (end == std::string::npos) {
        return std::nullopt;
    }
    const auto start = content->rfind('\n', end == 0 ? 0 : end - 1);
    const auto begin = start == std::string::npos || start >= end ? 0 : start + 1;
    const auto line = std::string_view{*content}.substr(begin, end - begin);
    const auto parsed = parse_line(line);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return Tip{.seq = parsed->seq, .hash = sha256(line)};
}

} // namespace internal

std::vector<std::uint32_t> list_days(const std::string& stream_dir) {
    std::vector<std::uint32_t> days;
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator{stream_dir, ec}) {
        const auto name = item.path().filename().string();
        if (name.size() <= internal::k_day_suffix.size() || !name.ends_with(internal::k_day_suffix)) {
            continue;
        }
        if (const auto day = parse_day(std::string_view{name}.substr(0, name.size() - internal::k_day_suffix.size()));
            day.has_value()) {
            days.push_back(*day);
        }
    }
    std::ranges::sort(days);
    return days;
}

std::string day_path(const std::string& stream_dir, std::uint32_t day) {
    return stream_dir + "/" + day_name(day) + std::string{internal::k_day_suffix};
}

std::string head_path(const std::string& stream_dir) {
    return stream_dir + "/HEAD";
}

std::optional<Tip> read_head(const std::string& stream_dir) {
    const auto content = internal::read_file(head_path(stream_dir));
    if (!content.has_value()) {
        return std::nullopt;
    }
    const std::string_view text{*content};
    const auto space = text.find(' ');
    const auto stop = text.find('\n');
    if (space == std::string_view::npos || stop == std::string_view::npos || stop < space) {
        return std::nullopt;
    }
    Tip head{};
    const auto [end, ec] = std::from_chars(text.data(), text.data() + space, head.seq);
    if (ec != std::errc{} || end != text.data() + space) {
        return std::nullopt;
    }
    const auto hash = digest_from_hex(text.substr(space + 1, stop - space - 1));
    if (!hash.has_value()) {
        return std::nullopt;
    }
    head.hash = *hash;
    return head;
}

ChainReport verify_day(const std::string& stream_dir, std::uint32_t day, VerifiedPrefix* prefix) {
    ChainReport report{.day = day};
    const auto days = list_days(stream_dir);
    const auto at = std::ranges::lower_bound(days, day);
    if (at == days.end() || *at != day) {
        return report;
    }
    report.found = true;
    const auto path = day_path(stream_dir, day);
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    // a file shorter than what was walked is a day cut back, which a walk from the start reports
    const VerifiedPrefix* earlier = prefix != nullptr && prefix->bytes != 0 && !ec && prefix->bytes <= size ? prefix
                                                                                                            : nullptr;
    const bool newest = at + 1 == days.end();
    // HEAD before the file: a live writer only moves it after the lines it names are there
    const auto head = newest ? read_head(stream_dir) : std::nullopt;
    const auto anchor = earlier != nullptr   ? std::optional{earlier->tip}
                        : at == days.begin() ? std::nullopt
                                             : internal::tail_of(day_path(stream_dir, *std::ranges::prev(at)));
    const auto content = internal::read_from(path, earlier != nullptr ? earlier->bytes : 0);
    if (!content.has_value()) {
        report.first_break = Break{.kind = BreakKind::Malformed};
        return report;
    }

    ChainWalk walk{anchor};
    std::optional<Digest> hash_at_head;
    if (earlier != nullptr && head.has_value() && head->seq == earlier->tip.seq) {
        hash_at_head = earlier->tip.hash;
    }
    std::string_view rest{*content};
    while (!rest.empty()) {
        const auto stop = rest.find('\n');
        if (stop == std::string_view::npos) {
            report.torn_tail_bytes = rest.size();
            break;
        }
        const auto line = rest.substr(0, stop);
        rest.remove_prefix(stop + 1);
        const auto parsed = parse_line(line);
        if (!parsed.has_value()) {
            walk.malformed();
            break;
        }
        const auto hash = sha256(line);
        if (!walk.line(parsed->seq, parsed->prev, hash)) {
            break;
        }
        if (head.has_value() && parsed->seq == head->seq) {
            hash_at_head = hash;
        }
        report.recoveries += parsed->action == k_action_recovery ? 1U : 0U;
        report.overflows += parsed->action == k_action_overflow ? 1U : 0U;
    }

    const auto walked = earlier != nullptr ? earlier->report.lines : 0;
    report.lines = walked + walk.lines();
    report.first_seq = earlier != nullptr ? earlier->report.first_seq : walk.first_seq();
    report.last_seq = walk.lines() != 0 ? walk.tip().seq : earlier != nullptr ? earlier->tip.seq : 0;
    report.head = walk.lines() != 0 ? walk.tip().hash : earlier != nullptr ? earlier->tip.hash : report.head;
    report.anchored = earlier != nullptr ? earlier->report.anchored : walk.anchored();
    report.first_break = walk.first_break();
    if (report.first_break.has_value()) {
        report.first_break->line += walked;
    }
    if (earlier != nullptr) {
        report.recoveries += earlier->report.recoveries;
        report.overflows += earlier->report.overflows;
    }
    if (report.ok() && head.has_value()) {
        // an empty newest file is a rotation about to write its first line, and HEAD still names the day before
        const std::optional<std::uint64_t> last = report.lines != 0    ? std::optional{report.last_seq}
                                                  : anchor.has_value() ? std::optional{anchor->seq}
                                                                       : std::nullopt;
        const bool tail_gone = !last.has_value() || *last < head->seq;
        if (tail_gone || (hash_at_head.has_value() && *hash_at_head != head->hash)) {
            report.first_break = Break{.kind = BreakKind::Head,
                                       .line = report.lines,
                                       .expected_seq = head->seq,
                                       .found_seq = last.value_or(0)};
        }
        report.head_checked = tail_gone || hash_at_head.has_value();
    }
    if (prefix != nullptr && report.ok() && report.lines != 0) {
        const auto whole = content->size() - rest.size();
        prefix->bytes = (earlier != nullptr ? earlier->bytes : 0) + whole;
        prefix->tip = {.seq = report.last_seq, .hash = report.head};
        prefix->report = report;
    }
    return report;
}

std::vector<ChainReport> verify_all(const std::string& stream_dir) {
    std::vector<ChainReport> reports;
    for (const auto day : list_days(stream_dir)) {
        reports.push_back(verify_day(stream_dir, day));
        if (!reports.back().ok()) {
            break;
        }
    }
    return reports;
}

} // namespace raikiri::audit
