#include <raikiri/joe/routes.hpp>

#include <array>
#include <charconv>
#include <glaze/glaze.hpp>
#include <optional>
#include <string>
#include <vector>

namespace raikiri::joe {

// at namespace scope because glaze needs linkage to reflect
constexpr glz::opts k_opts{.skip_null_members = true};

struct NewParentBody {
    std::string symbol;
    std::string market{"perp"};
    std::string side;
    std::optional<std::string> qty{};
    std::optional<std::string> notional{};
    std::optional<std::string> limit{};
    std::string algo{"twap"};
    std::uint64_t horizon_s{0};
    std::uint32_t slices{1};
    std::uint32_t jitter_bp{0};
    std::uint32_t catchup_pct{200};
    std::string profile{"flat"};
    std::uint32_t pov_bp{0};
    std::optional<std::string> max_child_notional{};
};

struct ChildDto {
    std::string client_order_id;
    std::string instrument_id;
    std::string venue;
    std::string qty;
    std::string limit;
    std::string filled;
    std::optional<std::string> avg_price{};
    std::string fees;
    std::string sent;
    bool open{false};
};

struct ParentDto {
    std::string id;
    std::string symbol;
    std::string market;
    std::string side;
    std::string algo;
    std::string profile;
    std::string status;
    std::optional<std::string> reason{};
    std::string qty;
    std::string filled;
    std::string working;
    std::optional<std::string> limit{};
    std::optional<std::string> arrival{};
    std::optional<std::string> avg_price{};
    std::optional<std::string> market_vwap{};
    std::optional<std::string> slippage_arrival_bp{};
    std::optional<std::string> slippage_vwap_bp{};
    std::string fees;
    std::uint64_t horizon_s{0};
    std::uint32_t slices{0};
    std::uint32_t slices_fired{0};
    std::uint32_t pov_bp{0};
    std::string created;
    std::optional<std::string> started{};
    std::optional<std::string> ended{};
    std::uint64_t child_count{0};
    std::optional<std::vector<ChildDto>> children{};
};

struct ParentList {
    std::vector<ParentDto> items;
};

struct Started {
    std::string id;
};

namespace {

constexpr std::uint64_t k_second = 1'000'000'000ULL;

template <typename T> http::Response json(const T& value, int status = 200) {
    std::string out;
    if (const auto ec = glz::write<k_opts>(value, out); ec) {
        return http::problem(500, "serialisation failed");
    }
    return {.status = status, .content_type = "application/json", .body = std::move(out)};
}

[[nodiscard]] std::string decimal(FixedE9 value) {
    std::array<char, 32> buf{};
    return std::string{format_e9(value, buf.data(), buf.size())};
}

[[nodiscard]] std::optional<std::string> maybe(FixedE9 value) {
    if (value.is_undef()) {
        return std::nullopt;
    }
    return decimal(value);
}

[[nodiscard]] std::optional<std::string> maybe(UnixNanos ts) {
    if (ts.ns == 0) {
        return std::nullopt;
    }
    return std::to_string(ts.ns);
}

// positive is a cost: paid above the benchmark on a buy, below it on a sell
[[nodiscard]] std::optional<std::string> slippage_bp(Side side, FixedE9 avg, FixedE9 benchmark) {
    if (avg.is_undef() || benchmark.is_undef() || benchmark.raw <= 0) {
        return std::nullopt;
    }
    const auto diff = side == Side::Buy ? avg.raw - benchmark.raw : benchmark.raw - avg.raw;
    return decimal(FixedE9::from_raw(mul_div(diff, 10'000 * k_e9, benchmark.raw) / 1'000'000 * 1'000'000));
}

[[nodiscard]] ChildDto to_dto(const Child& child) {
    return {.client_order_id = std::to_string(child.id),
            .instrument_id = std::to_string(child.instrument.packed),
            .venue = std::string{to_string(child.instrument.venue())},
            .qty = decimal(child.qty()),
            .limit = decimal(child.limit),
            .filled = decimal(child.filled),
            .avg_price = child.filled.raw > 0
                             ? maybe(FixedE9::from_raw(mul_div(child.value.raw, k_e9, child.filled.raw)))
                             : std::nullopt,
            .fees = decimal(child.fees),
            .sent = std::to_string(child.sent.ns),
            .open = child.open};
}

[[nodiscard]] ParentDto to_dto(const Parent& parent, bool with_children, UnixNanos now) {
    const auto& spec = parent.spec();
    ParentDto out{.id = std::to_string(parent.id()),
                  .symbol = std::string{spec.listing.symbol.view()},
                  .market = spec.listing.market == MarketType::Spot ? "spot" : "perp",
                  .side = spec.side == Side::Buy ? "buy" : "sell",
                  .algo = std::string{to_string(spec.algo)},
                  .profile = std::string{to_string(spec.profile)},
                  .status = std::string{to_string(parent.status())},
                  .reason = parent.reason().empty() ? std::nullopt : std::optional<std::string>{parent.reason()},
                  .qty = decimal(parent.qty()),
                  .filled = decimal(parent.filled()),
                  .working = decimal(parent.working()),
                  .limit = maybe(spec.limit),
                  .arrival = maybe(parent.arrival()),
                  .avg_price = maybe(parent.avg_price()),
                  .market_vwap = maybe(parent.market_vwap()),
                  .slippage_arrival_bp = slippage_bp(spec.side, parent.avg_price(), parent.arrival()),
                  .slippage_vwap_bp = slippage_bp(spec.side, parent.avg_price(), parent.market_vwap()),
                  .fees = decimal(parent.fees()),
                  .horizon_s = spec.horizon_ns / k_second,
                  .slices = static_cast<std::uint32_t>(parent.times().size()),
                  .slices_fired = 0,
                  .pov_bp = spec.pov_bp,
                  .created = std::to_string(parent.created().ns),
                  .started = maybe(parent.started()),
                  .ended = maybe(parent.ended()),
                  .child_count = parent.children().size()};
    if (parent.status() != Status::Pending) {
        out.slices_fired = fired(parent.times(), terminal(parent.status()) ? parent.ended() : now);
    }
    if (with_children) {
        std::vector<ChildDto> rows;
        rows.reserve(parent.children().size());
        for (const auto& child : parent.children()) {
            rows.push_back(to_dto(child));
        }
        out.children = std::move(rows);
    }
    return out;
}

[[nodiscard]] std::optional<FixedE9> read_decimal(const std::optional<std::string>& text) {
    if (!text.has_value() || text->empty()) {
        return FixedE9::undef();
    }
    const auto parsed = parse_decimal_e9(*text);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return *parsed;
}

[[nodiscard]] std::expected<ParentSpec, std::string> read_spec(std::string_view body) {
    NewParentBody in{};
    if (const auto ec = glz::read_json(in, body); ec) {
        return std::unexpected{"the body is not a parent order: " + glz::format_error(ec, body)};
    }
    ParentSpec spec{};
    const auto symbol = SymbolText::from(in.symbol);
    if (!symbol.has_value()) {
        return std::unexpected{std::string{"the symbol is too long"}};
    }
    spec.listing.symbol = *symbol;
    if (in.market == "perp" || in.market == "spot") {
        spec.listing.market = in.market == "spot" ? MarketType::Spot : MarketType::Perp;
    } else {
        return std::unexpected{std::string{"market is perp or spot"}};
    }
    if (in.side != "buy" && in.side != "sell") {
        return std::unexpected{std::string{"side is buy or sell"}};
    }
    spec.side = in.side == "buy" ? Side::Buy : Side::Sell;
    const auto qty = read_decimal(in.qty);
    const auto notional = read_decimal(in.notional);
    const auto limit = read_decimal(in.limit);
    const auto cap = read_decimal(in.max_child_notional);
    if (!qty.has_value() || !notional.has_value() || !limit.has_value() || !cap.has_value()) {
        return std::unexpected{std::string{"qty, notional, limit and max_child_notional are decimals as text"}};
    }
    spec.qty = *qty;
    spec.notional = *notional;
    spec.limit = *limit;
    if (!cap->is_undef()) {
        spec.max_child_notional = *cap;
    }
    if (in.algo == "sor") {
        spec.algo = Algo::Sor;
    } else if (in.algo == "twap") {
        spec.algo = Algo::Twap;
    } else if (in.algo == "vwap") {
        spec.algo = Algo::Vwap;
    } else if (in.algo == "pov") {
        spec.algo = Algo::Pov;
    } else {
        return std::unexpected{std::string{"algo is sor, twap, vwap or pov"}};
    }
    if (in.profile == "flat" || in.profile == "u" || in.profile == "recorded") {
        spec.profile = in.profile == "u" ? Profile::U : in.profile == "recorded" ? Profile::Recorded : Profile::Flat;
    } else {
        return std::unexpected{std::string{"profile is flat, u or recorded"}};
    }
    if (in.horizon_s > 7ULL * 24 * 60 * 60) {
        return std::unexpected{std::string{"the horizon is at most a week"}};
    }
    spec.horizon_ns = in.horizon_s * k_second;
    spec.slices = in.slices;
    spec.jitter_bp = in.jitter_bp;
    spec.catchup_pct = in.catchup_pct;
    spec.pov_bp = in.pov_bp;
    return spec;
}

[[nodiscard]] http::Response refused(const Refusal& why) {
    switch (why.error) {
    case DeskError::BadSpec:
        return http::problem(422, "bad parent order", why.detail);
    case DeskError::NotRunning:
        return http::problem(503, "joe is not running", why.detail);
    case DeskError::NoSuchParent:
        return http::problem(404, "no such parent order", why.detail);
    case DeskError::Conflict:
        return http::problem(409, "not in a state for that", why.detail);
    }
    return http::problem(500, "unknown refusal");
}

[[nodiscard]] std::optional<std::uint64_t> read_id(std::string_view text) {
    std::uint64_t value = 0;
    const auto [stop, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || ec != std::errc{} || stop != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

} // namespace

std::optional<http::Response> JoeRoutes::operator()(const http::Request& req) const {
    if (!req.path.starts_with(k_prefix)) {
        return std::nullopt;
    }
    if (desk_ == nullptr) {
        return http::problem(503, "joe is not enabled", "start strategyd with --joe <strategy id>");
    }
    const auto tail = req.path.substr(k_prefix.size());
    if (tail == "parents") {
        if (req.method == "GET") {
            ParentList list;
            for (auto it = desk_->parents().rbegin(); it != desk_->parents().rend(); ++it) {
                list.items.push_back(to_dto(*it, false, desk_->now()));
            }
            return json(list);
        }
        if (req.method == "POST") {
            const auto spec = read_spec(req.body);
            if (!spec.has_value()) {
                return http::problem(400, "bad request", spec.error());
            }
            const auto started = desk_->start(*spec);
            if (!started.has_value()) {
                return refused(started.error());
            }
            return json(Started{.id = std::to_string(*started)}, 201);
        }
        return http::problem(405, "method not allowed");
    }
    constexpr std::string_view k_parents = "parents/";
    if (!tail.starts_with(k_parents)) {
        return http::problem(404, "no such path", req.path);
    }
    const auto rest = tail.substr(k_parents.size());
    const auto slash = rest.find('/');
    const auto id = read_id(rest.substr(0, slash));
    if (!id.has_value()) {
        return http::problem(404, "no such parent order", req.path);
    }
    if (slash == std::string_view::npos) {
        if (req.method != "GET") {
            return http::problem(405, "method not allowed");
        }
        const auto* parent = desk_->find(*id);
        if (parent == nullptr) {
            return http::problem(404, "no such parent order", req.path);
        }
        return json(to_dto(*parent, true, desk_->now()));
    }
    if (req.method != "POST") {
        return http::problem(405, "method not allowed");
    }
    const auto action = rest.substr(slash + 1);
    std::expected<void, Refusal> done;
    if (action == "cancel") {
        done = desk_->cancel(*id);
    } else if (action == "pause") {
        done = desk_->pause(*id);
    } else if (action == "resume") {
        done = desk_->resume(*id);
    } else {
        return http::problem(404, "no such action", action);
    }
    if (!done.has_value()) {
        return refused(done.error());
    }
    return json(to_dto(*desk_->find(*id), false, desk_->now()));
}

} // namespace raikiri::joe
