#include <raikiri/joe/routes.hpp>
#include <raikiri/joe/tca.hpp>

#include <array>
#include <charconv>
#include <glaze/glaze.hpp>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

namespace raikiri::joe {

// at namespace scope because glaze needs linkage to reflect
constexpr glz::opts k_opts{.skip_null_members = true};

struct IcebergBody {
    std::string show;
    std::uint32_t variance_bp{0};
    std::string refill{"complete"};
};

struct PeggedBody {
    std::string to{"primary"};
    std::int32_t offset_ticks{0};
    std::int32_t offset_bp{0};
    std::uint32_t threshold_ticks{1};
    std::uint64_t min_interval_ms{1000};
};

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
    std::uint32_t depth_share_bp{0};
    std::optional<std::string> depth_band_bp{};
    std::optional<IcebergBody> iceberg{};
    std::optional<PeggedBody> pegged{};
    std::optional<std::string> venue{};
    bool post_only{false};
    std::string client_tag{};
    std::optional<std::string> book{};
    std::string external_ref{};
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
    std::optional<std::string> acked{};
    std::optional<std::string> closed{};
    std::string end;
    bool open{false};
    std::string tif;
    bool cancelling{false};
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
    std::uint64_t children_retired{0};
    std::optional<std::vector<ChildDto>> children{};
    std::optional<std::string> venue{};
    bool post_only{false};
    std::optional<IcebergBody> iceberg{};
    std::optional<PeggedBody> pegged{};
    std::optional<std::string> working_price{};
    std::optional<std::string> hold{};
    std::optional<std::string> client_tag{};
    // zero is joe's own book, which has no number joe knows of
    std::uint16_t book_id{0};
    std::optional<std::string> book{};
    std::optional<std::string> book_kind{};
    std::optional<std::string> external_ref{};
    std::string version{};
};

struct MidDto {
    std::uint64_t seq{0};
    std::string ts;
    std::string mid;
    std::optional<std::string> vwap{};
};

struct FillDto {
    std::uint64_t seq{0};
    std::string ts;
    std::string client_order_id;
    std::string venue;
    std::string qty;
    std::string price;
    std::string fee;
};

struct RepriceDto {
    std::uint64_t seq{0};
    std::string ts;
    std::string client_order_id;
    std::optional<std::string> venue{};
    std::optional<std::string> price{};
    std::string qty;
    std::string why;
};

struct PlanPoint {
    std::string ts;
    std::string qty;
};

struct VenueTcaDto {
    std::string venue;
    std::uint32_t children{0};
    std::uint32_t filled_children{0};
    std::uint32_t cancelled{0};
    std::uint32_t rejected{0};
    std::uint32_t acked{0};
    std::string sent;
    std::string filled;
    std::string value;
    std::string fees;
    std::optional<std::string> slippage_arrival_bp{};
    std::optional<std::string> fees_bp{};
    std::optional<std::string> ack_p50_ns{};
    std::optional<std::string> ack_p90_ns{};
    std::optional<std::string> ack_max_ns{};
};

struct TraceDto {
    std::string id;
    std::uint64_t seq{0};
    std::string interval_ns;
    std::uint64_t mids_dropped{0};
    std::uint64_t fills_dropped{0};
    std::uint64_t reprices_dropped{0};
    std::vector<MidDto> mids{};
    std::vector<FillDto> fills{};
    std::vector<RepriceDto> reprices{};
    std::optional<std::vector<PlanPoint>> plan{};
    std::vector<VenueTcaDto> venues{};
};

struct TcaDto {
    std::uint32_t parents{0};
    std::vector<VenueTcaDto> venues;
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
            .acked = maybe(child.acked),
            .closed = maybe(child.closed),
            .end = std::string{to_string(child.end)},
            .open = child.open,
            .tif = child.tif == ChildTif::Ioc   ? "ioc"
                   : child.tif == ChildTif::Gtc ? "gtc"
                                                : "post_only",
            .cancelling = child.open && child.cancel_sent.ns != 0};
}

[[nodiscard]] std::optional<std::string> nanos(std::uint32_t count, std::uint64_t ns) {
    return count == 0 ? std::nullopt : std::optional<std::string>{std::to_string(ns)};
}

[[nodiscard]] std::vector<VenueTcaDto> to_dto(const Tca& tca) {
    std::vector<VenueTcaDto> out;
    for (const auto& v : tca.venues()) {
        out.push_back({.venue = std::string{to_string(v.venue)},
                       .children = v.children,
                       .filled_children = v.filled_children,
                       .cancelled = v.cancelled,
                       .rejected = v.rejected,
                       .acked = v.acked,
                       .sent = decimal(v.sent),
                       .filled = decimal(v.filled),
                       .value = decimal(v.value),
                       .fees = decimal(v.fees),
                       .slippage_arrival_bp = maybe(bp_of(v.cost, v.benchmark)),
                       .fees_bp = maybe(bp_of(v.fees, v.value)),
                       .ack_p50_ns = nanos(v.acked, v.ack_p50_ns),
                       .ack_p90_ns = nanos(v.acked, v.ack_p90_ns),
                       .ack_max_ns = nanos(v.acked, v.ack_max_ns)});
    }
    return out;
}

// only what is newer than `since`; the plan rides along on a full read
[[nodiscard]] TraceDto to_trace(const Parent& parent, std::uint64_t since) {
    const auto& trace = parent.trace();
    TraceDto out{.id = std::to_string(parent.id()),
                 .seq = trace.seq(),
                 .interval_ns = std::to_string(trace.interval_ns()),
                 .mids_dropped = trace.mids().dropped(),
                 .fills_dropped = trace.fills().dropped(),
                 .reprices_dropped = trace.reprices().dropped()};
    for (std::size_t i = 0; i < trace.mids().size(); ++i) {
        const auto& m = trace.mids().at(i);
        if (m.seq > since) {
            out.mids.push_back(
                {.seq = m.seq, .ts = std::to_string(m.ts.ns), .mid = decimal(m.mid), .vwap = maybe(m.vwap)});
        }
    }
    for (std::size_t i = 0; i < trace.fills().size(); ++i) {
        const auto& f = trace.fills().at(i);
        if (f.seq > since) {
            out.fills.push_back({.seq = f.seq,
                                 .ts = std::to_string(f.ts.ns),
                                 .client_order_id = std::to_string(f.child_id),
                                 .venue = std::string{to_string(f.instrument.venue())},
                                 .qty = decimal(f.qty),
                                 .price = decimal(f.price),
                                 .fee = decimal(f.fee)});
        }
    }
    for (std::size_t i = 0; i < trace.reprices().size(); ++i) {
        const auto& r = trace.reprices().at(i);
        if (r.seq > since) {
            out.reprices.push_back({.seq = r.seq,
                                    .ts = std::to_string(r.ts.ns),
                                    .client_order_id = std::to_string(r.child_id),
                                    .venue = r.instrument.valid()
                                                 ? std::optional<std::string>{to_string(r.instrument.venue())}
                                                 : std::nullopt,
                                    .price = maybe(r.price),
                                    .qty = decimal(r.qty),
                                    .why = std::string{to_string(r.why)}});
        }
    }
    if (since == 0) {
        std::vector<PlanPoint> plan;
        const auto times = parent.times();
        plan.reserve(times.size());
        for (std::size_t i = 0; i < times.size(); ++i) {
            const auto target = cumulative_target(parent.qty(), parent.weights(), static_cast<std::uint32_t>(i + 1));
            plan.push_back({.ts = std::to_string(times[i].ns), .qty = decimal(target)});
        }
        out.plan = std::move(plan);
    }
    Tca tca;
    tca.add(parent);
    out.venues = to_dto(tca);
    return out;
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
                  .child_count = parent.child_count(),
                  .children_retired = parent.retired_count(),
                  .post_only = spec.post_only};
    out.version = std::to_string(parent.version());
    if (spec.venue != VenueId::Invalid) {
        out.venue = std::string{to_string(spec.venue)};
    }
    if (spec.algo == Algo::Iceberg) {
        out.iceberg = IcebergBody{.show = decimal(spec.iceberg.show),
                                  .variance_bp = spec.iceberg.variance_bp,
                                  .refill = std::string{to_string(spec.iceberg.refill)}};
    }
    if (spec.algo == Algo::Pegged) {
        out.pegged = PeggedBody{.to = std::string{to_string(spec.peg.to)},
                                .offset_ticks = spec.peg.offset_ticks,
                                .offset_bp = spec.peg.offset_bp,
                                .threshold_ticks = spec.peg.threshold_ticks,
                                .min_interval_ms = spec.peg.min_interval_ns / 1'000'000};
    }
    if (const auto* live = parent.live(); resting(spec.algo) && live != nullptr) {
        out.working_price = decimal(live->limit);
    }
    if (parent.slot().hold != Hold::None && !terminal(parent.status())) {
        out.hold = parent.slot().hold == Hold::Crossed ? "crossed" : "stale";
    }
    if (!spec.client_tag.empty()) {
        out.client_tag = spec.client_tag;
    }
    out.book_id = spec.book;
    if (!spec.external_ref.empty()) {
        out.external_ref = spec.external_ref;
    }
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
    spec.depth_share_bp = in.depth_share_bp;
    const auto band = read_decimal(in.depth_band_bp);
    if (!band.has_value()) {
        return std::unexpected{std::string{"depth_band_bp is a decimal as text"}};
    }
    if (!band->is_undef()) {
        spec.depth_band_bp = *band;
    }
    if (in.algo == "sor") {
        spec.algo = Algo::Sor;
    } else if (in.algo == "twap") {
        spec.algo = Algo::Twap;
    } else if (in.algo == "vwap") {
        spec.algo = Algo::Vwap;
    } else if (in.algo == "pov") {
        spec.algo = Algo::Pov;
    } else if (in.algo == "iceberg") {
        spec.algo = Algo::Iceberg;
    } else if (in.algo == "pegged") {
        spec.algo = Algo::Pegged;
    } else {
        return std::unexpected{std::string{"algo is sor, twap, vwap, pov, iceberg or pegged"}};
    }
    if (in.iceberg.has_value() != (spec.algo == Algo::Iceberg)) {
        return std::unexpected{std::string{"iceberg goes with algo iceberg, and that algo needs it"}};
    }
    if (in.pegged.has_value() && spec.algo != Algo::Pegged) {
        return std::unexpected{std::string{"pegged goes with algo pegged"}};
    }
    if ((in.venue.has_value() || in.post_only) && !resting(spec.algo)) {
        return std::unexpected{std::string{"venue and post_only go with a resting algo"}};
    }
    spec.post_only = in.post_only;
    if (in.venue.has_value()) {
        spec.venue = venue_from_string(*in.venue);
        if (spec.venue == VenueId::Invalid) {
            return std::unexpected{"no such venue: " + *in.venue};
        }
    }
    if (in.iceberg.has_value()) {
        const auto show = parse_decimal_e9(in.iceberg->show);
        if (!show.has_value()) {
            return std::unexpected{std::string{"iceberg.show is a decimal as text"}};
        }
        if (in.iceberg->refill != "complete" && in.iceberg->refill != "fill") {
            return std::unexpected{std::string{"iceberg.refill is complete or fill"}};
        }
        spec.iceberg = {.show = *show,
                        .variance_bp = in.iceberg->variance_bp,
                        .refill = in.iceberg->refill == "fill" ? Refill::OnFill : Refill::OnComplete};
    }
    if (in.pegged.has_value()) {
        const auto& peg = *in.pegged;
        if (peg.to != "primary" && peg.to != "mid" && peg.to != "market") {
            return std::unexpected{std::string{"pegged.to is primary, mid or market"}};
        }
        if (peg.min_interval_ms > 3'600'000) {
            return std::unexpected{std::string{"pegged.min_interval_ms is at most an hour"}};
        }
        spec.peg = {.to = peg.to == "mid"      ? PegTo::Mid
                          : peg.to == "market" ? PegTo::Market
                                               : PegTo::Primary,
                    .offset_ticks = peg.offset_ticks,
                    .offset_bp = peg.offset_bp,
                    .threshold_ticks = peg.threshold_ticks,
                    .min_interval_ns = peg.min_interval_ms * 1'000'000ULL};
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
    spec.client_tag = std::move(in.client_tag);
    spec.external_ref = std::move(in.external_ref);
    return spec;
}

// read apart from the spec, because a name means nothing until the registry answers
[[nodiscard]] std::string read_book(std::string_view body) {
    NewParentBody in{};
    (void)glz::read_json(in, body);
    return in.book.value_or(std::string{});
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
    const auto labelled = [this](const Parent& parent, bool with_children) {
        auto dto = to_dto(parent, with_children, desk_->now());
        if (const auto* book = books_ == nullptr ? nullptr : books_->find(dto.book_id); book != nullptr) {
            dto.book = book->name;
            dto.book_kind = std::string{to_string(book->kind)};
        }
        return dto;
    };
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
            const auto tag = http::query_param(req.query, "client_tag");
            std::optional<std::uint16_t> wanted_book;
            if (const auto text = http::query_param(req.query, "book"); !text.empty()) {
                const auto* book = books_ == nullptr ? nullptr : books_->find(http::percent_decoded(text));
                if (book == nullptr) {
                    return http::problem(404, "no such book", text);
                }
                wanted_book = book->id;
            }
            for (const auto& parent : std::views::reverse(desk_->parents())) {
                if ((tag.empty() || parent.spec().client_tag == tag) &&
                    (!wanted_book.has_value() || parent.spec().book == *wanted_book)) {
                    list.items.push_back(labelled(parent, false));
                }
            }
            return json(list);
        }
        if (req.method == "POST") {
            auto spec = read_spec(req.body);
            if (!spec.has_value()) {
                return http::problem(400, "bad request", spec.error());
            }
            const auto named = read_book(req.body);
            const auto default_book = http::query_param(req.query, "default_book");
            if (const auto name = named.empty() ? default_book : std::string_view{named}; !name.empty()) {
                if (books_ == nullptr) {
                    return http::problem(503, "no book registry", "strategyd holds no registry to resolve a book by");
                }
                const auto book = books_->resolve(name);
                if (!book.has_value()) {
                    return http::problem(book.error() == books::BookError::NotFound ? 404 : 422,
                                         "bad book",
                                         books::to_string(book.error()));
                }
                spec->book = book->id;
            }
            const bool held = desk_->find_tag(spec->client_tag) != nullptr;
            const auto started = desk_->start(*spec);
            if (!started.has_value()) {
                return refused(started.error());
            }
            return json(Started{.id = std::to_string(*started)}, held ? 200 : 201);
        }
        return http::problem(405, "method not allowed");
    }
    if (tail == "tca") {
        if (req.method != "GET") {
            return http::problem(405, "method not allowed");
        }
        Tca tca;
        TcaDto out{};
        for (const auto& parent : desk_->parents()) {
            if (parent.status() != Status::Pending) {
                tca.add(parent);
                ++out.parents;
            }
        }
        out.venues = to_dto(tca);
        return json(out);
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
        return json(labelled(*parent, true));
    }
    const auto action = rest.substr(slash + 1);
    if (action == "trace") {
        if (req.method != "GET") {
            return http::problem(405, "method not allowed");
        }
        const auto* parent = desk_->find(*id);
        if (parent == nullptr) {
            return http::problem(404, "no such parent order", req.path);
        }
        const auto since_text = http::query_param(req.query, "since");
        const auto since = since_text.empty() ? std::optional<std::uint64_t>{0} : read_id(since_text);
        if (!since.has_value()) {
            return http::problem(400, "bad request", "since is a trace seq");
        }
        return json(to_trace(*parent, *since));
    }
    if (req.method != "POST") {
        return http::problem(405, "method not allowed");
    }
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
    return json(labelled(*desk_->find(*id), false));
}

} // namespace raikiri::joe
