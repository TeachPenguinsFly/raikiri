#include <raikiri/optiondata/board.hpp>
#include <raikiri/optionrisk/routes.hpp>

#include <array>
#include <charconv>
#include <glaze/glaze.hpp>
#include <string>
#include <vector>

namespace raikiri::optionrisk {

namespace dto {

using Text = std::optional<std::string>;

struct Position {
    std::string account{};
    std::string venue{};
    std::string instrument_id{};
    std::string symbol{};
    std::string underlying{};
    std::string expiry{};
    std::string expiry_date{};
    std::string strike{};
    std::string put_call{};
    std::string premium_unit{};
    std::string contracts{};
    std::string contract_value{};
    Text units{};
    Text days_to_expiry{};
    Text entry{};
    Text mark{};
    std::string mark_source{};
    Text mark_ts{};
    Text vol{};
    Text forward{};
    Text rate{};
    Text delta_coin{};
    Text delta_adjusted_coin{};
    Text delta_usd{};
    Text gamma{};
    Text gamma_usd_1pct{};
    Text vega_usd{};
    Text theta_usd{};
    Text premium{};
    Text premium_usd{};
    Text value{};
    Text value_usd{};
    Text unrealized{};
    Text unrealized_usd{};
    Text realized{};
    Text realized_usd{};
    std::vector<std::string> flags{};
    std::string as_of{};
};

struct Awaiting {
    std::string account{};
    std::string venue{};
    std::string instrument_id{};
    std::string symbol{};
    std::string underlying{};
    std::string expiry{};
    std::string contracts{};
};

struct Settled {
    std::string account{};
    std::string venue{};
    std::string instrument_id{};
    std::string symbol{};
    std::string underlying{};
    std::string expiry{};
    std::string strike{};
    std::string put_call{};
    std::string premium_unit{};
    Text units{};
    Text entry{};
    std::string price{};
    std::string price_source{};
    std::string price_ts{};
    std::string settled_at{};
    bool from_last_seen{false};
    bool late{false};
    Text payoff{};
    Text proceeds{};
    Text proceeds_usd{};
    Text realized{};
    Text realized_usd{};
};

struct Totals {
    Text delta_coin{};
    Text delta_adjusted_coin{};
    std::string delta_usd{};
    Text gamma{};
    std::string gamma_usd_1pct{};
    std::string vega_usd{};
    std::string theta_usd{};
    std::string premium_usd{};
    std::string value_usd{};
    std::string unrealized_usd{};
    std::string realized_usd{};
    std::uint32_t positions{0};
    std::uint32_t incomplete{0};
    std::uint32_t settled{0};
};

struct Underlying {
    std::string underlying{};
    Totals totals{};
};

struct Expiry {
    std::string underlying{};
    std::string expiry{};
    std::string expiry_date{};
    Text days_to_expiry{};
    Totals totals{};
};

struct Risk {
    std::string as_of{};
    bool stale{false};
    bool realized_available{false};
    std::vector<Position> positions{};
    std::vector<Awaiting> awaiting_settlement{};
    std::vector<Settled> settled{};
    std::vector<Underlying> by_underlying{};
    std::vector<Expiry> by_expiry{};
    Totals total{};
};

struct Cell {
    std::string spot{};
    std::string vol{};
    Text value_usd{};
    Text pnl_usd{};
    bool worst{false};
};

struct Worst {
    std::string spot{};
    std::string vol{};
    std::string pnl_usd{};
};

struct Scenario {
    std::string underlying{};
    std::vector<std::string> spot{};
    std::vector<std::string> vol{};
    std::vector<Cell> cells{};
    std::optional<Worst> worst{};
    std::string base_value_usd{};
    std::uint32_t positions{0};
    std::vector<std::string> excluded{};
};

} // namespace dto

namespace internal {

constexpr std::size_t k_max_shocks = 21;
const auto k_hundred = FixedE9::from_raw(100 * k_e9);

[[nodiscard]] std::string digits(std::uint64_t value) {
    std::array<char, 24> buf{};
    const auto [stop, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
    return ec == std::errc{} ? std::string{buf.data(), static_cast<std::size_t>(stop - buf.data())} : std::string{};
}

[[nodiscard]] std::string decimal(FixedE9 value) {
    if (value.is_undef()) {
        return {};
    }
    std::array<char, 32> buf{};
    return std::string{format_e9(value, buf.data(), buf.size())};
}

[[nodiscard]] dto::Text text(FixedE9 value) {
    return value.is_undef() ? std::nullopt : dto::Text{decimal(value)};
}

[[nodiscard]] std::string_view unit_name(OptionUnit unit) noexcept {
    return unit == OptionUnit::Base ? "base" : (unit == OptionUnit::Quote ? "quote" : "unknown");
}

[[nodiscard]] std::string_view put_call_name(PutCall put_call) noexcept {
    return put_call == PutCall::Call ? "call" : "put";
}

[[nodiscard]] std::string_view mark_name(MarkFrom from) noexcept {
    switch (from) {
    case MarkFrom::Venue:
        return "venue";
    case MarkFrom::Mid:
        return "mid";
    case MarkFrom::Model:
        return "model";
    case MarkFrom::None:
        break;
    }
    return "none";
}

[[nodiscard]] std::vector<std::string> flag_names(std::uint16_t flags) {
    constexpr std::array<std::pair<Flag, std::string_view>, 8> k_names{{{k_mark_missing, "mark_missing"},
                                                                        {k_mark_stale, "mark_stale"},
                                                                        {k_mark_model, "mark_model"},
                                                                        {k_greeks_missing, "greeks_missing"},
                                                                        {k_greeks_stale, "greeks_stale"},
                                                                        {k_position_stale, "position_stale"},
                                                                        {k_entry_missing, "entry_missing"},
                                                                        {k_rate_missing, "rate_missing"}}};
    std::vector<std::string> out;
    for (const auto& [flag, name] : k_names) {
        if ((flags & flag) != 0U) {
            out.emplace_back(name);
        }
    }
    return out;
}

[[nodiscard]] std::string venue_name(InstrumentId id) {
    return std::string{to_string(id.venue())};
}

[[nodiscard]] dto::Position position_of(const PositionRisk& p) {
    const auto& h = p.holding;
    return {.account = h.account,
            .venue = venue_name(h.instrument),
            .instrument_id = digits(h.instrument.packed),
            .symbol = std::string{h.symbol.view()},
            .underlying = std::string{h.key.underlying.view()},
            .expiry = digits(h.key.expiry.ns),
            .expiry_date = optiondata::expiry_date(h.key.expiry),
            .strike = decimal(h.key.strike),
            .put_call = std::string{put_call_name(h.key.put_call)},
            .premium_unit = std::string{unit_name(h.premium)},
            .contracts = decimal(h.contracts),
            .contract_value = decimal(h.contract_value),
            .units = text(p.units),
            .days_to_expiry = text(p.days_to_expiry),
            .entry = text(h.entry),
            .mark = text(p.market.mark),
            .mark_source = std::string{mark_name(p.market.mark_from)},
            .mark_ts = p.market.mark_ts.ns == 0 ? std::nullopt : dto::Text{digits(p.market.mark_ts.ns)},
            .vol = text(p.vol),
            .forward = text(p.market.forward),
            .rate = text(p.market.rate),
            .delta_coin = text(p.delta_coin),
            .delta_adjusted_coin = text(p.delta_adjusted_coin),
            .delta_usd = text(p.delta_usd),
            .gamma = text(p.gamma),
            .gamma_usd_1pct = text(p.gamma_usd_1pct),
            .vega_usd = text(p.vega_usd),
            .theta_usd = text(p.theta_usd),
            .premium = text(p.premium),
            .premium_usd = text(p.premium_usd),
            .value = text(p.value),
            .value_usd = text(p.value_usd),
            .unrealized = text(p.unrealized),
            .unrealized_usd = text(p.unrealized_usd),
            .realized = text(p.realized),
            .realized_usd = text(p.realized_usd),
            .flags = flag_names(p.flags),
            .as_of = digits(h.as_of.ns)};
}

[[nodiscard]] dto::Settled settled_of(const SettledPosition& s) {
    const auto& h = s.holding;
    return {.account = h.account,
            .venue = venue_name(h.instrument),
            .instrument_id = digits(h.instrument.packed),
            .symbol = std::string{h.symbol.view()},
            .underlying = std::string{h.key.underlying.view()},
            .expiry = digits(h.key.expiry.ns),
            .strike = decimal(h.key.strike),
            .put_call = std::string{put_call_name(h.key.put_call)},
            .premium_unit = std::string{unit_name(h.premium)},
            .units = text(s.units),
            .entry = text(h.entry),
            .price = decimal(s.at.price),
            .price_source = s.at.by == SettledBy::Venue ? "venue" : "index",
            .price_ts = digits(s.at.ts.ns),
            .settled_at = digits(s.settled_at.ns),
            .from_last_seen = s.from_last_seen,
            .late = s.late,
            .payoff = text(s.payoff),
            .proceeds = text(s.proceeds),
            .proceeds_usd = text(s.proceeds_usd),
            .realized = text(s.realized),
            .realized_usd = text(s.realized_usd)};
}

[[nodiscard]] dto::Totals totals_of(const Totals& t, bool one_underlying) {
    return {.delta_coin = one_underlying ? text(t.delta_coin) : std::nullopt,
            .delta_adjusted_coin = one_underlying ? text(t.delta_adjusted_coin) : std::nullopt,
            .delta_usd = decimal(t.delta_usd),
            .gamma = one_underlying ? text(t.gamma) : std::nullopt,
            .gamma_usd_1pct = decimal(t.gamma_usd_1pct),
            .vega_usd = decimal(t.vega_usd),
            .theta_usd = decimal(t.theta_usd),
            .premium_usd = decimal(t.premium_usd),
            .value_usd = decimal(t.value_usd),
            .unrealized_usd = decimal(t.unrealized_usd),
            .realized_usd = decimal(t.realized_usd),
            .positions = t.positions,
            .incomplete = t.incomplete,
            .settled = t.settled};
}

[[nodiscard]] dto::Risk risk_of(const RiskView& view) {
    dto::Risk out{.as_of = digits(view.as_of.ns),
                  .realized_available = view.realized_known,
                  .total = totals_of(view.aggregates.total, false)};
    out.positions.reserve(view.positions.size());
    constexpr std::uint16_t k_doubt = k_mark_missing | k_mark_stale | k_greeks_missing | k_greeks_stale |
                                      k_position_stale;
    for (const auto& p : view.positions) {
        out.positions.push_back(position_of(p));
        out.stale = out.stale || (p.flags & k_doubt) != 0U;
    }
    out.stale = out.stale || !view.awaiting_settlement.empty();
    for (const auto& h : view.awaiting_settlement) {
        out.awaiting_settlement.push_back({.account = h.account,
                                           .venue = venue_name(h.instrument),
                                           .instrument_id = digits(h.instrument.packed),
                                           .symbol = std::string{h.symbol.view()},
                                           .underlying = std::string{h.key.underlying.view()},
                                           .expiry = digits(h.key.expiry.ns),
                                           .contracts = decimal(h.contracts)});
    }
    for (const auto& s : view.settled) {
        out.settled.push_back(settled_of(s));
    }
    for (const auto& u : view.aggregates.by_underlying) {
        out.by_underlying.push_back(
            {.underlying = std::string{u.underlying.view()}, .totals = totals_of(u.totals, true)});
    }
    for (const auto& e : view.aggregates.by_expiry) {
        out.by_expiry.push_back({.underlying = std::string{e.underlying.view()},
                                 .expiry = digits(e.expiry.ns),
                                 .expiry_date = optiondata::expiry_date(e.expiry),
                                 .days_to_expiry = text(e.days_to_expiry),
                                 .totals = totals_of(e.totals, true)});
    }
    return out;
}

// the query's shocks are in percent and vol points; the domain's are fractions
[[nodiscard]] FixedE9 fraction(FixedE9 percent) noexcept {
    return FixedE9::from_raw(percent.raw / 100);
}

[[nodiscard]] std::string percent(FixedE9 shock) {
    return decimal(FixedE9::from_raw(shock.raw * 100));
}

[[nodiscard]] std::expected<std::vector<FixedE9>, std::string> shocks_of(std::string_view text,
                                                                         std::string_view fallback) {
    if (text.empty()) {
        text = fallback;
    }
    std::vector<FixedE9> out;
    while (!text.empty()) {
        const auto comma = text.find(',');
        const auto encoded = text.find("%2C");
        const bool plain = comma <= encoded;
        const auto stop = plain ? comma : encoded;
        const auto one = text.substr(0, stop);
        const auto parsed = parse_decimal_e9(one);
        if (!parsed.has_value() || parsed->raw <= -k_hundred.raw || parsed->raw > 10 * k_hundred.raw) {
            return std::unexpected(std::string{one});
        }
        out.push_back(fraction(*parsed));
        text = stop == std::string_view::npos ? std::string_view{} : text.substr(stop + (plain ? 1 : 3));
    }
    if (out.empty() || out.size() > k_max_shocks) {
        return std::unexpected("between 1 and 21 shocks");
    }
    return out;
}

[[nodiscard]] dto::Scenario scenario_of(std::string_view underlying, const ScenarioMatrix& m) {
    dto::Scenario out{
        .underlying = std::string{underlying}, .base_value_usd = decimal(m.base_value_usd), .positions = m.positions};
    for (const auto s : m.spot) {
        out.spot.push_back(percent(s));
    }
    for (const auto v : m.vol) {
        out.vol.push_back(percent(v));
    }
    out.cells.reserve(m.cells.size());
    for (std::size_t i = 0; i < m.cells.size(); ++i) {
        const auto& c = m.cells[i];
        const bool worst = m.worst == i;
        out.cells.push_back({.spot = percent(c.spot),
                             .vol = percent(c.vol),
                             .value_usd = text(c.value_usd),
                             .pnl_usd = text(c.pnl_usd),
                             .worst = worst});
        if (worst) {
            out.worst = dto::Worst{.spot = percent(c.spot), .vol = percent(c.vol), .pnl_usd = decimal(c.pnl_usd)};
        }
    }
    for (const auto id : m.excluded) {
        out.excluded.push_back(digits(id.packed));
    }
    return out;
}

template <typename T> http::Response json(const T& value) {
    std::string out;
    if (glz::write_json(value, out)) {
        return http::problem(500, "serialisation failed");
    }
    return {.status = 200, .content_type = "application/json", .body = std::move(out)};
}

} // namespace internal

std::optional<http::Response> RiskRoutes::operator()(const http::Request& req) const {
    if (req.path != "/v1/options/risk" && req.path != "/v1/options/risk/scenarios") {
        return std::nullopt;
    }
    if (req.method != "GET") {
        return http::problem(405, "method not allowed", "this path serves reads");
    }
    if (req.path == "/v1/options/risk") {
        return internal::json(internal::risk_of(risk_->view()));
    }
    const auto underlying = http::query_param(req.query, "underlying");
    if (underlying.empty()) {
        return http::problem(400, "underlying is required");
    }
    const auto spot = internal::shocks_of(http::query_param(req.query, "spot"), "-30,-20,-10,-5,0,5,10,20,30");
    if (!spot.has_value()) {
        return http::problem(400, "spot shocks are percent moves above -100", spot.error());
    }
    const auto vol = internal::shocks_of(http::query_param(req.query, "vol"), "-20,-10,-5,0,5,10,20");
    if (!vol.has_value()) {
        return http::problem(400, "vol shocks are vol points above -100", vol.error());
    }
    return internal::json(internal::scenario_of(underlying, risk_->scenarios(underlying, *spot, *vol)));
}

} // namespace raikiri::optionrisk
