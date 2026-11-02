#include <raikiri/optionrisk/short_book_routes.hpp>

#include <array>
#include <glaze/glaze.hpp>
#include <string>

namespace raikiri::optionrisk {
namespace internal {

namespace dto {
struct Row {
    std::uint16_t book_id{};
    std::string book{};
    std::string venue{};
    std::string asset{};
    std::int64_t short_lots{};
    std::int64_t working_lots{};
    std::optional<std::string> naked_usd{};
    std::optional<std::string> margin_held{};
    std::optional<std::string> margin_working{};
    std::optional<std::string> margin_available{};
    std::optional<std::string> asset_usd{};
    std::optional<std::string> book_naked_usd{};
    std::optional<std::string> vega_usd{};
    std::optional<std::string> gamma_usd_1pct{};
    std::optional<std::string> max_naked_short{};
    std::optional<std::string> max_short_vega{};
    std::optional<std::string> max_short_gamma{};
    bool reducing{};
};

struct Books {
    std::vector<Row> books{};
};
} // namespace dto

[[nodiscard]] std::optional<std::string> text(FixedE9 value) {
    if (value.is_undef()) {
        return std::nullopt;
    }
    std::array<char, 32> buf{};
    return std::string{format_e9(value, buf.data(), buf.size())};
}

} // namespace internal

std::optional<http::Response> ShortBookRoutes::operator()(const http::Request& req) const {
    if (req.path != "/options/short-books") {
        return std::nullopt;
    }
    if (req.method != "GET") {
        return http::problem(405, "method not allowed", "this path serves reads");
    }
    internal::dto::Books out{};
    for (const auto& row : rows_()) {
        out.books.push_back({.book_id = row.book_id,
                             .book = row.book,
                             .venue = row.venue,
                             .asset = row.asset,
                             .short_lots = row.short_lots,
                             .working_lots = row.working_lots,
                             .naked_usd = internal::text(row.naked_usd),
                             .margin_held = internal::text(row.margin_held),
                             .margin_working = internal::text(row.margin_working),
                             .margin_available = internal::text(row.margin_available),
                             .asset_usd = internal::text(row.asset_usd),
                             .book_naked_usd = internal::text(row.book_naked_usd),
                             .vega_usd = internal::text(row.vega_usd),
                             .gamma_usd_1pct = internal::text(row.gamma_usd_1pct),
                             .max_naked_short = internal::text(row.max_naked_short),
                             .max_short_vega = internal::text(row.max_short_vega),
                             .max_short_gamma = internal::text(row.max_short_gamma),
                             .reducing = row.reducing});
    }
    std::string body;
    if (glz::write_json(out, body)) {
        return http::problem(500, "serialisation failed");
    }
    return http::Response{.status = 200, .content_type = "application/json", .body = std::move(body)};
}

} // namespace raikiri::optionrisk
