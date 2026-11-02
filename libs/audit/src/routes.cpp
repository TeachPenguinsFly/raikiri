#include <raikiri/audit/routes.hpp>

#include <charconv>
#include <glaze/glaze.hpp>
#include <string>
#include <vector>

namespace raikiri::audit {

namespace dto {

struct Unparsed {
    std::string unparsed{};
};

struct Item {
    std::string stream{};
    std::string day{};
    std::string seq{};
    std::string ts{};
    std::string actor{};
    std::string action{};
    std::string subject{};
    glz::raw_json detail{};
    std::string prev_sha256{};
    std::string sha256{};
};

struct Page {
    std::vector<Item> items{};
    std::optional<std::string> next_cursor{};
};

struct FirstBreak {
    std::string kind{};
    std::string line{};
    std::string expected_seq{};
    std::string found_seq{};
};

struct Stream {
    std::string stream{};
    std::string state{};
    std::optional<std::string> day{};
    std::uint64_t days{0};
    std::string lines{};
    std::string first_seq{};
    std::string last_seq{};
    std::optional<std::string> head_sha256{};
    bool anchored{false};
    bool head_checked{false};
    std::uint64_t torn_tail_bytes{0};
    std::uint64_t recoveries{0};
    std::uint64_t overflows{0};
    std::optional<FirstBreak> first_break{};
};

struct Status {
    std::string as_of{};
    bool ok{true};
    std::vector<Stream> streams{};
};

} // namespace dto

namespace internal {

constexpr glz::opts k_opts{.skip_null_members = true};

template <typename T> [[nodiscard]] http::Response json(const T& value) {
    std::string out;
    if (const auto ec = glz::write<k_opts>(value, out); ec) {
        return http::problem(500, "serialisation failed");
    }
    return {.status = 200, .content_type = "application/json", .body = std::move(out)};
}

template <typename T> [[nodiscard]] std::optional<T> read_digits(std::string_view text) noexcept {
    T value{};
    const auto [stop, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || ec != std::errc{} || stop != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

// query_param leaves its value encoded
[[nodiscard]] std::string percent_decoded(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            unsigned code = 0;
            const auto [stop, ec] = std::from_chars(text.data() + i + 1, text.data() + i + 3, code, 16);
            if (ec == std::errc{} && stop == text.data() + i + 3) {
                out.push_back(static_cast<char>(code));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i] == '+' ? ' ' : text[i]);
    }
    return out;
}

// a line edited by hand can carry anything, and one bad detail must not spoil a whole answer
[[nodiscard]] std::string shown_detail(const std::string& detail) {
    if (!glz::validate_json(detail)) {
        return detail;
    }
    std::string out;
    if (glz::write_json(dto::Unparsed{.unparsed = detail}, out)) {
        return "{}";
    }
    return out;
}

[[nodiscard]] dto::Item to_dto(const AuditRecord& record) {
    return {.stream = record.stream,
            .day = day_name(record.day),
            .seq = std::to_string(record.seq),
            .ts = std::to_string(record.ts.ns),
            .actor = record.actor,
            .action = record.action,
            .subject = record.subject,
            .detail = glz::raw_json{shown_detail(record.detail_json)},
            .prev_sha256 = to_hex(record.prev),
            .sha256 = to_hex(record.hash)};
}

[[nodiscard]] std::string_view state_of(const ChainReport& report) noexcept {
    if (!report.found) {
        return "empty";
    }
    if (!report.ok()) {
        return "broken";
    }
    return report.recoveries != 0 || report.overflows != 0 ? "recovered" : "intact";
}

[[nodiscard]] dto::Stream to_dto(const StreamStatus& row) {
    const auto& report = row.report;
    dto::Stream out{.stream = row.stream,
                    .state = std::string{state_of(report)},
                    .days = row.days,
                    .lines = std::to_string(report.lines),
                    .first_seq = std::to_string(report.first_seq),
                    .last_seq = std::to_string(report.last_seq),
                    .anchored = report.anchored,
                    .head_checked = report.head_checked,
                    .torn_tail_bytes = report.torn_tail_bytes,
                    .recoveries = report.recoveries,
                    .overflows = report.overflows};
    if (report.found) {
        out.day = day_name(report.day);
    }
    if (report.lines != 0) {
        out.head_sha256 = to_hex(report.head);
    }
    if (report.first_break.has_value()) {
        out.first_break = dto::FirstBreak{.kind = std::string{to_string(report.first_break->kind)},
                                          .line = std::to_string(report.first_break->line),
                                          .expected_seq = std::to_string(report.first_break->expected_seq),
                                          .found_seq = std::to_string(report.first_break->found_seq)};
    }
    return out;
}

[[nodiscard]] http::Response query_route(const QueryAudit& query, const http::Request& req) {
    AuditQuery wanted{.actor = percent_decoded(http::query_param(req.query, "actor")),
                      .action = percent_decoded(http::query_param(req.query, "action")),
                      .subject = percent_decoded(http::query_param(req.query, "subject"))};
    if (const auto text = http::query_param(req.query, "since"); !text.empty()) {
        const auto value = read_digits<std::uint64_t>(text);
        if (!value.has_value()) {
            return http::problem(400, "bad since", "nanoseconds since the epoch");
        }
        wanted.since = UnixNanos{*value};
    }
    if (const auto text = http::query_param(req.query, "until"); !text.empty()) {
        const auto value = read_digits<std::uint64_t>(text);
        if (!value.has_value()) {
            return http::problem(400, "bad until", "nanoseconds since the epoch");
        }
        wanted.until = UnixNanos{*value};
    }
    if (const auto text = http::query_param(req.query, "limit"); !text.empty()) {
        const auto value = read_digits<std::size_t>(text);
        if (!value.has_value() || *value == 0 || *value > k_max_limit) {
            return http::problem(400, "bad limit", "a whole number from 1 to 1000");
        }
        wanted.limit = *value;
    }
    if (const auto text = http::query_param(req.query, "cursor"); !text.empty()) {
        wanted.after = parse_cursor(percent_decoded(text));
        if (!wanted.after.has_value()) {
            return http::problem(400, "bad cursor", "take it from next_cursor");
        }
    }
    const auto page = query(wanted);
    dto::Page out{};
    out.items.reserve(page.items.size());
    for (const auto& record : page.items) {
        out.items.push_back(to_dto(record));
    }
    if (page.next.has_value()) {
        out.next_cursor = format_cursor(*page.next);
    }
    return json(out);
}

[[nodiscard]] http::Response status_route(const GetChainStatus& status, const http::Request& req) {
    std::optional<std::uint32_t> day;
    if (const auto text = http::query_param(req.query, "day"); !text.empty()) {
        day = parse_day(text);
        if (!day.has_value()) {
            return http::problem(400, "bad day", "YYYY-MM-DD");
        }
    }
    const auto found = status(day);
    dto::Status out{.as_of = std::to_string(found.as_of.ns)};
    out.streams.reserve(found.streams.size());
    for (const auto& row : found.streams) {
        out.streams.push_back(to_dto(row));
        out.ok = out.ok && row.report.ok();
    }
    return json(out);
}

} // namespace internal

std::optional<http::Response> AuditRoutes::operator()(const http::Request& req) const {
    if (req.path != "/v1/audit" && req.path != "/v1/audit/status") {
        return std::nullopt;
    }
    if (req.method != "GET") {
        return http::problem(405, "method not allowed", "this path serves reads");
    }
    return req.path == "/v1/audit" ? internal::query_route(*query_, req) : internal::status_route(*status_, req);
}

} // namespace raikiri::audit
