#include <raikiri/bookrisk/routes.hpp>

#include <array>
#include <charconv>
#include <glaze/glaze.hpp>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::bookrisk {

constexpr glz::opts k_write{.skip_null_members = true};
constexpr glz::opts k_read{.error_on_unknown_keys = false};

struct LimitSetDto {
    std::string mode{};
    std::optional<std::string> value{};
};

struct LimitDto {
    LimitSetDto set{};
    std::string source{};
    std::string mode{};
    std::optional<std::string> effective{};
    std::optional<std::string> percent{};
};

struct RowDto {
    std::map<std::string, LimitDto> limits{};
    std::string daily_loss_action{};
    bool daily_loss_action_set{false};
};

struct WindowDto {
    std::string change{};
    std::uint64_t covered_s{0};
    bool past_level{false};
};

struct PnlDto {
    std::string realized{};
    std::string unrealized{};
    std::string total{};
    WindowDto change_1m{};
    WindowDto change_5m{};
    WindowDto change_1h{};
    std::string day{};
    bool past_daily{false};
    std::uint32_t positions{0};
    std::uint32_t unmarked{0};
    std::uint32_t unvalued{0};
};

struct BookDto {
    std::uint32_t id{0};
    std::string name{};
    std::string state{};
    std::string own_state{};
    std::optional<std::string> reason{};
    std::optional<std::string> by{};
    std::optional<std::string> since{};
    std::uint64_t open_orders{0};
    RowDto row{};
    PnlDto pnl{};
};

struct DeskDto {
    std::optional<std::string> capital{};
    std::optional<std::string> capital_by{};
    std::optional<std::string> capital_at{};
    std::optional<std::string> reported_balance{};
    std::uint32_t reported_accounts{0};
    std::uint32_t unvalued_assets{0};
    std::optional<std::string> capital_warning{};
    std::string desk_state{};
    std::optional<std::string> desk_reason{};
    std::optional<std::string> desk_by{};
    std::optional<std::string> desk_since{};
    RowDto defaults{};
    std::vector<BookDto> books{};
};

struct ChangeBody {
    std::string by{};
    std::string reason{};
    bool confirm{false};
};

struct CapitalBody {
    std::string capital{};
    std::string by{};
    std::string reason{};
    bool confirm{false};
};

struct LimitsBody {
    std::map<std::string, LimitSetDto> limits{};
    std::optional<std::string> daily_loss_action{};
    std::string by{};
    std::string reason{};
    bool confirm{false};
};

struct StateBody {
    std::string state{};
    std::string by{};
    std::string reason{};
    bool confirm{false};
};

struct KillAnswer {
    std::uint64_t cancelled{0};
};

struct RefusedDto {
    std::string type{"about:blank"};
    std::string title{};
    std::uint32_t status{0};
    std::string detail{};
    std::vector<std::string> raises{};
};

namespace internal {

// the login the proxy vouches for beats a name typed in the page
[[nodiscard]] std::string who(const http::Request& req, const std::string& typed) {
    return req.login.empty() ? typed : std::string{req.login};
}


[[nodiscard]] std::string text(FixedE9 value) {
    std::array<char, 32> buf{};
    return std::string{format_e9(value, buf.data(), buf.size())};
}

[[nodiscard]] std::optional<std::string> maybe(FixedE9 value) {
    return value.is_undef() ? std::optional<std::string>{} : std::optional{text(value)};
}

[[nodiscard]] std::optional<std::string> maybe(const std::string& value) {
    return value.empty() ? std::optional<std::string>{} : std::optional{value};
}

[[nodiscard]] std::optional<std::string> maybe(UnixNanos at) {
    return at.ns == 0 ? std::optional<std::string>{} : std::optional{std::to_string(at.ns)};
}

[[nodiscard]] RowDto to_dto(const RowView& row) {
    RowDto out{.daily_loss_action = std::string{to_string(row.loss_action)},
               .daily_loss_action_set = row.loss_action_set};
    for (const auto& info : k_params) {
        const auto& view = row.limits[static_cast<std::size_t>(info.param)];
        out.limits[std::string{info.name}] = {
            .set = {.mode = std::string{to_string(view.set.mode)}, .value = maybe(view.set.value)},
            .source = std::string{to_string(view.resolved.source)},
            .mode = std::string{to_string(view.resolved.mode)},
            .effective = maybe(view.resolved.effective),
            .percent = maybe(view.resolved.percent)};
    }
    return out;
}

[[nodiscard]] WindowDto to_dto(const WindowView& view) {
    return {.change = text(view.change), .covered_s = view.covered_s, .past_level = view.past_level};
}

[[nodiscard]] BookDto to_dto(const BookView& book) {
    return {.id = book.id,
            .name = book.name,
            .state = std::string{oms::to_string(book.state)},
            .own_state = std::string{oms::to_string(book.own.state)},
            .reason = maybe(book.own.reason),
            .by = maybe(book.own.by),
            .since = maybe(book.own.since),
            .open_orders = book.open_orders,
            .row = to_dto(book.row),
            .pnl = {.realized = text(book.pnl.realized),
                    .unrealized = text(book.pnl.unrealized),
                    .total = text(book.pnl.total),
                    .change_1m = to_dto(book.pnl.m1),
                    .change_5m = to_dto(book.pnl.m5),
                    .change_1h = to_dto(book.pnl.h1),
                    .day = text(book.pnl.day),
                    .past_daily = book.pnl.past_daily,
                    .positions = book.pnl.positions,
                    .unmarked = book.pnl.unmarked,
                    .unvalued = book.pnl.unvalued}};
}

[[nodiscard]] DeskDto to_dto(const DeskView& view) {
    DeskDto out{.capital = maybe(view.capital),
                .capital_by = maybe(view.capital_by),
                .capital_at = maybe(view.capital_at),
                .reported_balance = view.reported.accounts > 0 ? maybe(view.reported.dollars) : std::nullopt,
                .reported_accounts = view.reported.accounts,
                .unvalued_assets = view.reported.unvalued,
                .capital_warning = maybe(view.capital_warning),
                .desk_state = std::string{oms::to_string(view.desk.state)},
                .desk_reason = maybe(view.desk.reason),
                .desk_by = maybe(view.desk.by),
                .desk_since = maybe(view.desk.since),
                .defaults = to_dto(view.defaults)};
    out.books.reserve(view.books.size());
    for (const auto& book : view.books) {
        out.books.push_back(to_dto(book));
    }
    return out;
}

template <typename T> [[nodiscard]] http::Response answer(const T& value) {
    std::string body;
    if (glz::write<k_write>(value, body)) {
        return http::problem(500, "serialisation failed");
    }
    return {.status = 200, .content_type = "application/json", .body = std::move(body)};
}

[[nodiscard]] http::Response refused(const DeskRefusal& why) {
    const auto status = [&]() -> std::uint32_t {
        switch (why.error) {
        case DeskError::Invalid:
            return 422;
        case DeskError::NeedsConfirm:
            return 409;
        case DeskError::NoSuchBook:
            return 404;
        case DeskError::NotDurable:
            return 503;
        }
        return 500;
    }();
    if (why.error != DeskError::NeedsConfirm) {
        return http::problem(static_cast<int>(status), "refused", why.detail);
    }
    std::string body;
    if (glz::write<k_write>(
            RefusedDto{.title = "needs confirm", .status = status, .detail = why.detail, .raises = why.raises}, body)) {
        return http::problem(500, "serialisation failed");
    }
    return {.status = static_cast<int>(status), .content_type = "application/problem+json", .body = std::move(body)};
}

[[nodiscard]] std::optional<BookId> book_of(std::string_view text) noexcept {
    BookId id{};
    const auto* end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, id);
    return parsed.ec == std::errc{} && parsed.ptr == end ? std::optional{id} : std::nullopt;
}

[[nodiscard]] std::expected<LimitPatch, std::string> patch_of(const LimitsBody& body) {
    LimitPatch out{};
    for (const auto& [name, set] : body.limits) {
        const auto param = param_from(name);
        if (!param.has_value()) {
            return std::unexpected{"no limit is called " + name};
        }
        const auto mode = limit_mode_from(set.mode);
        if (!mode.has_value()) {
            return std::unexpected{name + ": mode is inherit, off, absolute or percent"};
        }
        auto value = FixedE9::undef();
        if (set.value.has_value()) {
            const auto parsed = parse_decimal_e9(*set.value);
            if (!parsed.has_value()) {
                return std::unexpected{name + ": value is a decimal as text"};
            }
            value = *parsed;
        }
        out.limits.emplace_back(*param, Limit{.mode = *mode, .value = value});
    }
    if (body.daily_loss_action.has_value()) {
        out.loss_action = loss_action_from(*body.daily_loss_action);
        if (!out.loss_action.has_value()) {
            return std::unexpected{"daily_loss_action is off, reducing or halted"};
        }
    }
    return out;
}

} // namespace internal

std::optional<http::Response> RiskRoutes::operator()(const http::Request& req) const {
    if (req.path != "/risk" && !req.path.starts_with("/risk/")) {
        return std::nullopt;
    }
    const auto overview = [this] { return internal::answer(internal::to_dto(desk_->view())); };
    const auto done = [&overview](const std::expected<void, DeskRefusal>& result) {
        return result.has_value() ? overview() : internal::refused(result.error());
    };
    if (req.method == "GET") {
        if (req.path == "/risk") {
            return overview();
        }
        if (constexpr std::string_view k_books = "/risk/books/"; req.path.starts_with(k_books)) {
            const auto found = desk_->book_view(req.path.substr(k_books.size()));
            return found.has_value() ? internal::answer(internal::to_dto(*found))
                                     : http::problem(404, "no such book", req.path);
        }
        return http::problem(404, "no such path", req.path);
    }
    if (req.method != "POST") {
        return http::problem(405, "method not allowed");
    }
    if (req.path == "/risk/capital") {
        CapitalBody body{};
        if (glz::read<k_read>(body, req.body)) {
            return http::problem(400, "malformed body");
        }
        const auto capital = parse_decimal_e9(body.capital);
        if (!capital.has_value()) {
            return http::problem(422, "capital is a decimal as text");
        }
        return done(desk_->set_capital(*capital, {.by = internal::who(req, body.by), .reason = body.reason, .confirm = body.confirm}));
    }
    if (req.path == "/risk/defaults") {
        LimitsBody body{};
        if (glz::read<k_read>(body, req.body)) {
            return http::problem(400, "malformed body");
        }
        const auto patch = internal::patch_of(body);
        if (!patch.has_value()) {
            return http::problem(422, "refused", patch.error());
        }
        return done(desk_->set_defaults(*patch, {.by = internal::who(req, body.by), .reason = body.reason, .confirm = body.confirm}));
    }
    if (req.path == "/risk/kill") {
        ChangeBody body{};
        if (glz::read<k_read>(body, req.body)) {
            return http::problem(400, "malformed body");
        }
        const auto killed = desk_->kill(std::nullopt, {.by = internal::who(req, body.by), .reason = body.reason, .confirm = body.confirm});
        return killed.has_value() ? internal::answer(KillAnswer{.cancelled = *killed})
                                  : internal::refused(killed.error());
    }
    if (req.path == "/risk/resume") {
        ChangeBody body{};
        if (glz::read<k_read>(body, req.body)) {
            return http::problem(400, "malformed body");
        }
        return done(desk_->resume_desk({.by = internal::who(req, body.by), .reason = body.reason, .confirm = body.confirm}));
    }
    constexpr std::string_view k_books = "/risk/books/";
    if (!req.path.starts_with(k_books)) {
        return http::problem(404, "no such path", req.path);
    }
    const auto tail = req.path.substr(k_books.size());
    const auto slash = tail.find('/');
    const auto book = slash == std::string_view::npos ? std::nullopt : internal::book_of(tail.substr(0, slash));
    if (!book.has_value()) {
        return http::problem(404, "no such path", req.path);
    }
    const auto verb = tail.substr(slash + 1);
    if (verb == "limits") {
        LimitsBody body{};
        if (glz::read<k_read>(body, req.body)) {
            return http::problem(400, "malformed body");
        }
        const auto patch = internal::patch_of(body);
        if (!patch.has_value()) {
            return http::problem(422, "refused", patch.error());
        }
        return done(desk_->set_limits(*book, *patch, {.by = internal::who(req, body.by), .reason = body.reason, .confirm = body.confirm}));
    }
    if (verb == "state") {
        StateBody body{};
        if (glz::read<k_read>(body, req.body)) {
            return http::problem(400, "malformed body");
        }
        const auto state = oms::book_state_from(body.state);
        if (!state.has_value()) {
            return http::problem(422, "state is active or reducing");
        }
        return done(desk_->set_state(*book, *state, {.by = internal::who(req, body.by), .reason = body.reason, .confirm = body.confirm}));
    }
    if (verb == "kill") {
        ChangeBody body{};
        if (glz::read<k_read>(body, req.body)) {
            return http::problem(400, "malformed body");
        }
        const auto killed = desk_->kill(book, {.by = internal::who(req, body.by), .reason = body.reason, .confirm = body.confirm});
        return killed.has_value() ? internal::answer(KillAnswer{.cancelled = *killed})
                                  : internal::refused(killed.error());
    }
    return http::problem(404, "no such path", req.path);
}

} // namespace raikiri::bookrisk
