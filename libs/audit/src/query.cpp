#include <raikiri/audit/query.hpp>

#include <algorithm>
#include <charconv>
#include <map>
#include <tuple>

namespace raikiri::audit {

namespace internal {

struct Hit {
    AuditRecord record{};
    std::string line{};
};

[[nodiscard]] bool matches(std::string_view wanted, std::string_view value) noexcept {
    if (wanted.empty()) {
        return true;
    }
    return wanted.back() == '*' ? value.starts_with(wanted.substr(0, wanted.size() - 1)) : value == wanted;
}

[[nodiscard]] bool newer(const Position& a, const Position& b) noexcept {
    return std::tuple{b.day, b.ts, a.stream, b.seq} < std::tuple{a.day, a.ts, b.stream, a.seq};
}

[[nodiscard]] Position position_of(const AuditRecord& record) {
    return {.day = record.day, .ts = record.ts.ns, .stream = record.stream, .seq = record.seq};
}

template <typename T> [[nodiscard]] bool read_number(std::string_view& text, T& value) noexcept {
    const auto stop = text.find(':');
    const auto field = text.substr(0, stop);
    const auto [end, ec] = std::from_chars(field.data(), field.data() + field.size(), value);
    if (ec != std::errc{} || end != field.data() + field.size() || stop == std::string_view::npos) {
        return false;
    }
    text.remove_prefix(stop + 1);
    return true;
}

} // namespace internal

std::string format_cursor(const Position& position) {
    return std::to_string(position.day) + ":" + std::to_string(position.ts) + ":" + std::to_string(position.seq) + ":" +
           position.stream;
}

std::optional<Position> parse_cursor(std::string_view text) {
    Position out{};
    if (!internal::read_number(text, out.day) || !internal::read_number(text, out.ts) ||
        !internal::read_number(text, out.seq) || text.empty()) {
        return std::nullopt;
    }
    out.stream = std::string{text};
    return out;
}

AuditPage QueryAudit::operator()(const AuditQuery& query) const {
    std::map<std::uint32_t, std::vector<std::string>, std::greater<>> by_day;
    for (auto& stream : store_->streams()) {
        for (const auto day : store_->days(stream)) {
            by_day[day].push_back(stream);
        }
    }

    const std::size_t wanted = query.limit + 1;
    std::vector<internal::Hit> page;
    const auto order = [](const internal::Hit& a, const internal::Hit& b) {
        return internal::newer(internal::position_of(a.record), internal::position_of(b.record));
    };
    for (const auto& [day, streams] : by_day) {
        if (page.size() >= wanted) {
            break;
        }
        // a file's day is never before its entries' day, and only a late drain puts an entry past midnight
        if ((query.since.has_value() && day < day_of(*query.since)) ||
            (query.until.has_value() && day > day_of(*query.until) + 1) ||
            (query.after.has_value() && day > query.after->day)) {
            continue;
        }
        std::vector<internal::Hit> hits;
        for (const auto& stream : streams) {
            store_->scan_day(stream, day, [&](const EntryView& entry) {
                if ((query.since.has_value() && entry.ts < *query.since) ||
                    (query.until.has_value() && entry.ts >= *query.until) ||
                    !internal::matches(query.actor, entry.actor) || !internal::matches(query.action, entry.action) ||
                    !internal::matches(query.subject, entry.subject)) {
                    return;
                }
                if (query.after.has_value() &&
                    !internal::newer(*query.after,
                                     Position{.day = day, .ts = entry.ts.ns, .stream = stream, .seq = entry.seq})) {
                    return;
                }
                hits.push_back({.record = {.stream = stream,
                                           .day = day,
                                           .seq = entry.seq,
                                           .ts = entry.ts,
                                           .actor = std::string{entry.actor},
                                           .action = std::string{entry.action},
                                           .subject = std::string{entry.subject},
                                           .detail_json = std::string{entry.detail_json},
                                           .prev = entry.prev},
                                .line = std::string{entry.line}});
                // a day can hold far more matches than a page wants
                if (hits.size() >= (wanted * 2) + 1024) {
                    std::ranges::nth_element(hits, hits.begin() + static_cast<std::ptrdiff_t>(wanted), order);
                    hits.resize(wanted);
                }
            });
        }
        std::ranges::sort(hits, order);
        for (auto& hit : hits) {
            if (page.size() >= wanted) {
                break;
            }
            page.push_back(std::move(hit));
        }
    }

    AuditPage out{};
    const bool more = page.size() > query.limit;
    if (more) {
        page.resize(query.limit);
    }
    out.items.reserve(page.size());
    for (auto& hit : page) {
        hit.record.hash = sha256(hit.line);
        out.items.push_back(std::move(hit.record));
    }
    if (more && !out.items.empty()) {
        out.next = internal::position_of(out.items.back());
    }
    return out;
}

ChainStatus GetChainStatus::operator()(std::optional<std::uint32_t> day) const {
    ChainStatus out{.as_of = wall_()};
    for (auto& stream : store_->streams()) {
        const auto days = store_->days(stream);
        StreamStatus row{.stream = stream, .days = days.size()};
        if (!days.empty()) {
            row.report = store_->verify(stream, day.value_or(days.back()));
        }
        out.streams.push_back(std::move(row));
    }
    return out;
}

} // namespace raikiri::audit
