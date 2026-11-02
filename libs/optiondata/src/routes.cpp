#include <raikiri/optiondata/routes.hpp>

#include <array>
#include <charconv>
#include <glaze/glaze.hpp>
#include <string>
#include <vector>

namespace raikiri::optiondata {

namespace dto {

using Text = std::optional<std::string>;

struct VenueCount {
    std::string venue{};
    std::uint32_t options{0};
};

struct Expiry {
    std::string expiry{};
    std::string date{};
    std::uint32_t strikes{0};
    std::vector<std::string> venues{};
};

struct Underlying {
    std::string underlying{};
    std::vector<VenueCount> venues{};
    std::vector<Expiry> expiries{};
};

struct Quote {
    std::string venue{};
    std::string instrument_id{};
    std::string symbol{};
    std::string premium_unit{};
    Text bid{};
    Text ask{};
    Text bid_size{};
    Text ask_size{};
    Text mark{};
    Text bid_usd{};
    Text ask_usd{};
    Text mark_usd{};
    Text bid_iv{};
    Text ask_iv{};
    Text mark_iv{};
    std::string iv_source{};
    Text delta{};
    Text gamma{};
    Text vega{};
    Text theta{};
    std::string greeks_source{};
    Text greeks_unit{};
    Text open_interest{};
    Text volume_24h{};
    std::string ts_event{};
};

struct Best {
    std::string venue{};
    std::string usd{};
    Text iv{};
    Text size{};
};

struct Side {
    std::vector<Quote> venues{};
    std::optional<Best> best_bid{};
    std::optional<Best> best_ask{};
};

struct Row {
    std::string strike{};
    Side call{};
    Side put{};
};

struct VenueRef {
    std::string venue{};
    Text index{};
    Text underlying{};
};

struct Chain {
    std::string underlying{};
    std::string expiry{};
    std::string date{};
    std::string as_of{};
    Text years{};
    std::uint64_t seconds_to_expiry{0};
    Text forward{};
    std::string forward_source{};
    Text atm_strike{};
    std::vector<VenueRef> venues{};
    std::vector<Row> rows{};
};

struct Svi {
    std::string a{};
    std::string b{};
    std::string rho{};
    std::string m{};
    std::string sigma{};
};

struct Fit {
    Svi svi{};
    std::uint32_t quotes{0};
    std::uint32_t dropped{0};
    Text svi_rmse{};
    Text ssvi_rmse{};
    Text inside{};
};

struct Point {
    std::string strike{};
    std::string vol{};
};

struct Smile {
    std::string venue{};
    std::string strike{};
    std::string put_call{};
    std::string mid_iv{};
    Text bid_iv{};
    Text ask_iv{};
};

struct Residual {
    std::string venue{};
    std::uint32_t quotes{0};
    std::string mean{};
    Text rms{};
};

struct SurfaceExpiry {
    std::string expiry{};
    std::string date{};
    Text years{};
    Text forward{};
    std::string forward_source{};
    Text atm_iv{};
    Text rr25{};
    Text bf25{};
    Text call25_strike{};
    Text put25_strike{};
    std::optional<Fit> fit{};
    std::vector<Point> fitted{};
    std::vector<Smile> smile{};
    std::vector<Residual> residuals{};
};

struct Ssvi {
    std::string rho{};
    std::string eta{};
    std::string gamma{};
};

struct Surface {
    std::string underlying{};
    std::string as_of{};
    std::string model{};
    Text rmse{};
    std::optional<Ssvi> ssvi{};
    bool calendar_repair{false};
    std::uint32_t butterfly_points{0};
    std::uint32_t calendar_points{0};
    std::vector<Residual> venues{};
    std::vector<SurfaceExpiry> expiries{};
};

} // namespace dto

namespace internal {

[[nodiscard]] std::string digits(std::uint64_t value) {
    std::array<char, 24> buf{};
    const auto [stop, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
    return ec == std::errc{} ? std::string{buf.data(), static_cast<std::size_t>(stop - buf.data())} : std::string{};
}

[[nodiscard]] std::string decimal(FixedE9 value) {
    std::array<char, 32> buf{};
    return std::string{format_e9(value, buf.data(), buf.size())};
}

[[nodiscard]] dto::Text text(FixedE9 value) {
    return value.is_undef() ? std::nullopt : dto::Text{decimal(value)};
}

[[nodiscard]] std::string_view unit_name(OptionUnit unit) noexcept {
    switch (unit) {
    case OptionUnit::Base:
        return "base";
    case OptionUnit::Quote:
        return "quote";
    case OptionUnit::Unknown:
        break;
    }
    return "unknown";
}

[[nodiscard]] std::string_view figure_name(Figure figure) noexcept {
    switch (figure) {
    case Figure::Venue:
        return "venue";
    case Figure::Model:
        return "model";
    case Figure::None:
        break;
    }
    return "none";
}

[[nodiscard]] std::string_view forward_name(ForwardFrom from) noexcept {
    switch (from) {
    case ForwardFrom::Parity:
        return "parity";
    case ForwardFrom::Venue:
        return "venue";
    case ForwardFrom::Index:
        return "index";
    case ForwardFrom::None:
        break;
    }
    return "none";
}

[[nodiscard]] std::string_view model_name(options::SurfaceModel model) noexcept {
    switch (model) {
    case options::SurfaceModel::Ssvi:
        return "ssvi";
    case options::SurfaceModel::Svi:
        return "svi";
    case options::SurfaceModel::None:
        break;
    }
    return "none";
}

[[nodiscard]] dto::Quote quote_of(const QuoteView& v) {
    return {.venue = std::string{to_string(v.venue)},
            .instrument_id = digits(v.instrument.packed),
            .symbol = std::string{v.symbol.view()},
            .premium_unit = std::string{unit_name(v.premium)},
            .bid = text(v.bid),
            .ask = text(v.ask),
            .bid_size = text(v.bid_size),
            .ask_size = text(v.ask_size),
            .mark = text(v.mark),
            .bid_usd = text(v.bid_usd),
            .ask_usd = text(v.ask_usd),
            .mark_usd = text(v.mark_usd),
            .bid_iv = text(v.bid_iv),
            .ask_iv = text(v.ask_iv),
            .mark_iv = text(v.mark_iv),
            .iv_source = std::string{figure_name(v.iv)},
            .delta = text(v.delta),
            .gamma = text(v.gamma),
            .vega = text(v.vega),
            .theta = text(v.theta),
            .greeks_source = std::string{figure_name(v.greeks)},
            .greeks_unit = v.greeks == Figure::None ? dto::Text{} : dto::Text{std::string{unit_name(v.greeks_unit)}},
            .open_interest = text(v.open_interest),
            .volume_24h = text(v.volume_24h),
            .ts_event = digits(v.ts.ns)};
}

[[nodiscard]] std::optional<dto::Best> best_of(const Best& best) {
    if (best.venue == VenueId::Invalid) {
        return std::nullopt;
    }
    return dto::Best{.venue = std::string{to_string(best.venue)},
                     .usd = decimal(best.usd),
                     .iv = text(best.iv),
                     .size = text(best.size)};
}

[[nodiscard]] dto::Side side_of(const SideView& side) {
    dto::Side out{.best_bid = best_of(side.bid), .best_ask = best_of(side.ask)};
    out.venues.reserve(side.venues.size());
    for (const auto& v : side.venues) {
        out.venues.push_back(quote_of(v));
    }
    return out;
}

[[nodiscard]] dto::Chain chain_of(const ChainView& chain) {
    dto::Chain out{.underlying = std::string{chain.underlying.view()},
                   .expiry = digits(chain.expiry.ns),
                   .date = expiry_date(chain.expiry),
                   .as_of = digits(chain.as_of.ns),
                   .years = text(chain.years),
                   .seconds_to_expiry = chain.expiry.ns > chain.as_of.ns
                                            ? (chain.expiry.ns - chain.as_of.ns) / 1'000'000'000ULL
                                            : 0,
                   .forward = text(chain.forward.value),
                   .forward_source = std::string{forward_name(chain.forward.from)},
                   .atm_strike = text(chain.atm_strike)};
    for (const auto& ref : chain.venues) {
        out.venues.push_back(
            {.venue = std::string{to_string(ref.venue)}, .index = text(ref.index), .underlying = text(ref.underlying)});
    }
    out.rows.reserve(chain.rows.size());
    for (const auto& row : chain.rows) {
        out.rows.push_back({.strike = decimal(row.strike), .call = side_of(row.call), .put = side_of(row.put)});
    }
    return out;
}

[[nodiscard]] dto::SurfaceExpiry expiry_of(const ExpiryView& e) {
    dto::SurfaceExpiry out{.expiry = digits(e.expiry.ns),
                           .date = expiry_date(e.expiry),
                           .years = text(e.years),
                           .forward = text(e.forward.value),
                           .forward_source = std::string{forward_name(e.forward.from)},
                           .atm_iv = text(e.atm_iv),
                           .rr25 = text(e.rr25),
                           .bf25 = text(e.bf25),
                           .call25_strike = text(e.call25_strike),
                           .put25_strike = text(e.put25_strike)};
    if (e.fit.has_value()) {
        const auto& f = *e.fit;
        out.fit = dto::Fit{.svi = {.a = decimal(f.svi.a),
                                   .b = decimal(f.svi.b),
                                   .rho = decimal(f.svi.rho),
                                   .m = decimal(f.svi.m),
                                   .sigma = decimal(f.svi.sigma)},
                           .quotes = f.quotes,
                           .dropped = f.dropped,
                           .svi_rmse = text(f.svi_rmse),
                           .ssvi_rmse = text(f.ssvi_rmse),
                           .inside = text(f.inside)};
    }
    for (const auto& p : e.fitted) {
        out.fitted.push_back({.strike = decimal(p.strike), .vol = decimal(p.vol)});
    }
    for (const auto& s : e.smile) {
        out.smile.push_back({.venue = std::string{to_string(s.venue)},
                             .strike = decimal(s.strike),
                             .put_call = s.put_call == PutCall::Call ? "call" : "put",
                             .mid_iv = decimal(s.mid_iv),
                             .bid_iv = text(s.bid_iv),
                             .ask_iv = text(s.ask_iv)});
    }
    for (const auto& r : e.residuals) {
        out.residuals.push_back(
            {.venue = std::string{to_string(r.venue)}, .quotes = r.quotes, .mean = decimal(r.mean)});
    }
    return out;
}

[[nodiscard]] dto::Surface surface_of(const SurfaceView& s) {
    dto::Surface out{.underlying = std::string{s.underlying.view()},
                     .as_of = digits(s.as_of.ns),
                     .model = std::string{model_name(s.model)},
                     .rmse = text(s.rmse),
                     .calendar_repair = s.calendar_repair,
                     .butterfly_points = s.butterfly_points,
                     .calendar_points = s.calendar_points};
    if (s.model != options::SurfaceModel::None) {
        out.ssvi = dto::Ssvi{.rho = decimal(s.ssvi.rho), .eta = decimal(s.ssvi.eta), .gamma = decimal(s.ssvi.gamma)};
    }
    for (const auto& v : s.venues) {
        out.venues.push_back({.venue = std::string{to_string(v.venue)},
                              .quotes = v.quotes,
                              .mean = decimal(v.mean),
                              .rms = text(v.rms)});
    }
    for (const auto& e : s.expiries) {
        out.expiries.push_back(expiry_of(e));
    }
    return out;
}

[[nodiscard]] dto::Underlying underlying_of(const UnderlyingListing& u) {
    dto::Underlying out{.underlying = std::string{u.underlying.view()}};
    for (const auto& v : u.venues) {
        out.venues.push_back({.venue = std::string{to_string(v.venue)}, .options = v.options});
    }
    for (const auto& e : u.expiries) {
        dto::Expiry expiry{.expiry = digits(e.expiry.ns), .date = expiry_date(e.expiry), .strikes = e.strikes};
        for (const auto venue : e.venues) {
            expiry.venues.emplace_back(to_string(venue));
        }
        out.expiries.push_back(std::move(expiry));
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

// a comma list of venue names; empty is every venue
[[nodiscard]] std::expected<std::vector<VenueId>, std::string> venues_of(std::string_view text) {
    std::vector<VenueId> out;
    while (!text.empty()) {
        const auto comma = text.find(',');
        const auto name = text.substr(0, comma);
        const auto venue = venue_from_string(name);
        if (venue == VenueId::Invalid) {
            return std::unexpected(std::string{name});
        }
        out.push_back(venue);
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
    }
    return out;
}

} // namespace internal

std::optional<http::Response> OptionRoutes::operator()(const http::Request& req) const {
    if (!req.path.starts_with(k_options_prefix)) {
        return std::nullopt;
    }
    if (req.method != "GET") {
        return http::problem(405, "method not allowed", "this path serves reads");
    }
    if (req.path == "/v1/options/underlyings") {
        std::vector<dto::Underlying> out;
        for (const auto& u : (*underlyings_)()) {
            out.push_back(internal::underlying_of(u));
        }
        return internal::json(out);
    }
    const auto underlying = http::query_param(req.query, "underlying");
    const auto venues = internal::venues_of(http::query_param(req.query, "venue"));
    if (!venues.has_value()) {
        return http::problem(400, "unknown venue", venues.error());
    }
    if (req.path == "/v1/options/chain") {
        const auto expiry = http::query_param(req.query, "expiry");
        if (underlying.empty() || expiry.empty()) {
            return http::problem(400, "underlying and expiry are required");
        }
        const auto chain = (*chain_)({.underlying = underlying, .expiry = expiry, .venues = *venues});
        if (!chain.has_value()) {
            return chain.error() == ChainError::NoUnderlying
                       ? http::problem(404, "no options on that underlying", underlying)
                       : http::problem(404, "no such expiry", expiry);
        }
        return internal::json(internal::chain_of(*chain));
    }
    if (req.path == "/v1/options/surface") {
        if (underlying.empty()) {
            return http::problem(400, "underlying is required");
        }
        const auto surface = (*surface_)(underlying, *venues);
        if (!surface.has_value()) {
            return http::problem(404, "no options on that underlying", underlying);
        }
        return internal::json(internal::surface_of(*surface));
    }
    return http::problem(404, "no such path", req.path);
}

} // namespace raikiri::optiondata
