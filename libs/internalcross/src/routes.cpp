#include <raikiri/internalcross/presentation/routes.hpp>

#include <array>
#include <glaze/glaze.hpp>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::internalcross {

constexpr glz::opts k_write{.skip_null_members = true};
constexpr glz::opts k_read{.error_on_unknown_keys = false};

struct BookPolicyDto {
    std::string book{};
    bool enabled{false};
    std::int64_t fee_bp{0};
    std::vector<std::string> instruments{};
};

struct BookVolumeDto {
    std::uint32_t book_id{0};
    std::string book{};
    std::string notional{};
};

struct LiabilityDto {
    std::string instrument_id{};
    std::string symbol{};
    std::string client_long_lots{};
    std::string client_short_lots{};
    std::string hedge_lots{};
    std::string other_lots{};
    std::string book_sum_lots{};
    std::string offset_lots{};
    std::optional<std::string> venue_lots{};
};

struct StatsDto {
    std::uint64_t crosses{0};
    std::uint64_t against_clients{0};
    std::uint64_t against_desk{0};
    std::string notional{};
    std::string venue_cost_saved{};
    std::string fees{};
    std::map<std::string, std::uint64_t> refused{};
};

struct OverviewDto {
    bool enabled{false};
    bool against_desk{false};
    std::int64_t max_spread_bp{0};
    std::uint64_t max_quote_age_ms{0};
    std::uint64_t hold_ms{0};
    std::vector<BookPolicyDto> books{};
    StatsDto stats{};
    std::vector<BookVolumeDto> volume_by_book{};
    std::vector<LiabilityDto> liability{};
};

struct GlobalBody {
    std::optional<bool> enabled{};
    std::optional<bool> against_desk{};
    std::optional<std::int64_t> max_spread_bp{};
    std::optional<std::uint64_t> max_quote_age_ms{};
    std::optional<std::uint64_t> hold_ms{};
    std::string by{};
    std::string reason{};
    bool confirm{false};
};

struct BookBody {
    bool enabled{false};
    std::int64_t fee_bp{0};
    std::vector<std::string> instruments{};
    std::string by{};
    std::string reason{};
    bool confirm{false};
};

struct RefusedDto {
    std::string type{"about:blank"};
    std::string title{};
    std::uint32_t status{0};
    std::string detail{};
    std::vector<std::string> raises{};
};

namespace internal {

[[nodiscard]] std::string text(FixedE9 value) {
    std::array<char, 32> buf{};
    return std::string{format_e9(value, buf.data(), buf.size())};
}

template <typename T> [[nodiscard]] http::Response answer(const T& value, int status = 200) {
    http::Response out{};
    out.status = status;
    if (glz::write<k_write>(value, out.body)) {
        return http::problem(500, "cannot serialise the answer");
    }
    return out;
}

[[nodiscard]] http::Response refused(const ControlRefusal& refusal) {
    const auto status = refusal.error == ControlError::NeedsConfirm ? 409
                        : refusal.error == ControlError::NotDurable ? 503
                                                                    : 422;
    return answer(RefusedDto{.title = refusal.error == ControlError::NeedsConfirm ? "needs confirmation" : "refused",
                             .status = static_cast<std::uint32_t>(status),
                             .detail = refusal.detail,
                             .raises = refusal.raises},
                  status);
}

} // namespace internal

std::optional<http::Response> CrossRoutes::operator()(const http::Request& req) const {
    constexpr std::string_view k_root = "/risk/internal";
    constexpr std::string_view k_books = "/risk/internal/books/";
    if (req.path != k_root && !req.path.starts_with(k_books)) {
        return std::nullopt;
    }
    const auto overview = [this] {
        const auto& settings = control_->settings();
        const auto& stats = control_->desk().stats();
        OverviewDto out{.enabled = settings.enabled,
                        .against_desk = settings.against_desk,
                        .max_spread_bp = settings.max_spread_bp,
                        .max_quote_age_ms = settings.max_quote_age_ms,
                        .hold_ms = settings.hold_ms,
                        .stats = {.crosses = stats.crosses,
                                  .against_clients = stats.against_clients,
                                  .against_desk = stats.against_desk,
                                  .notional = internal::text(stats.notional),
                                  .venue_cost_saved = internal::text(stats.saved),
                                  .fees = internal::text(stats.fees)}};
        for (const auto& [name, policy] : settings.books) {
            out.books.push_back(
                {.book = name, .enabled = policy.enabled, .fee_bp = policy.fee_bp, .instruments = policy.instruments});
        }
        for (std::size_t i = 0; i < k_refusal_count; ++i) {
            if (stats.refused[i] != 0) {
                out.stats.refused[std::string{to_string(static_cast<Refusal>(i))}] = stats.refused[i];
            }
        }
        for (const auto& [book, notional] : stats.notional_by_book) {
            out.volume_by_book.push_back(
                {.book_id = book, .book = book_name_(book), .notional = internal::text(notional)});
        }
        for (const auto& view : liabilities_()) {
            out.liability.push_back(
                {.instrument_id = std::to_string(view.row.instrument.packed),
                 .symbol = view.symbol,
                 .client_long_lots = std::to_string(view.row.client_long),
                 .client_short_lots = std::to_string(view.row.client_short),
                 .hedge_lots = std::to_string(view.row.hedge),
                 .other_lots = std::to_string(view.row.others),
                 .book_sum_lots = std::to_string(view.row.book_sum()),
                 .offset_lots = std::to_string(view.row.offset()),
                 .venue_lots = view.venue.has_value() ? std::optional{std::to_string(*view.venue)} : std::nullopt});
        }
        return internal::answer(out);
    };
    if (req.method == "GET") {
        return req.path == k_root ? overview() : http::problem(404, "no such path", req.path);
    }
    if (req.method != "POST") {
        return http::problem(405, "method not allowed");
    }
    const auto who = [&req](const std::string& typed) { return req.login.empty() ? typed : std::string{req.login}; };
    if (req.path == k_root) {
        GlobalBody body{};
        if (glz::read<k_read>(body, req.body)) {
            return http::problem(400, "malformed body");
        }
        const auto done = control_->set({.enabled = body.enabled,
                                         .max_spread_bp = body.max_spread_bp,
                                         .max_quote_age_ms = body.max_quote_age_ms,
                                         .hold_ms = body.hold_ms,
                                         .against_desk = body.against_desk},
                                        {.by = who(body.by), .reason = body.reason, .confirm = body.confirm});
        return done.has_value() ? overview() : internal::refused(done.error());
    }
    BookBody body{};
    if (glz::read<k_read>(body, req.body)) {
        return http::problem(400, "malformed body");
    }
    const auto done = control_->set_book(
        std::string{req.path.substr(k_books.size())},
        {.enabled = body.enabled, .fee_bp = body.fee_bp, .instruments = body.instruments},
        {.by = who(body.by), .reason = body.reason, .confirm = body.confirm});
    return done.has_value() ? overview() : internal::refused(done.error());
}

} // namespace raikiri::internalcross
