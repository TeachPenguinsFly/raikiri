#include <raikiri/reference/routes.hpp>

#include <cstdio>
#include <glaze/glaze.hpp>
#include <string>
#include <vector>

namespace raikiri::reference {

namespace dto {

struct Cuts {
    std::uint32_t empty{0};
    std::uint32_t crossed{0};
    std::uint32_t wide{0};
    std::uint32_t stale{0};
    std::uint32_t no_rate{0};
    std::uint32_t duplicate{0};
    std::uint32_t warming{0};
    std::uint32_t outlier{0};
};

struct Item {
    std::string asset{};
    bool refused{true};
    std::optional<std::string> mid{};
    std::optional<std::string> spot_mid{};
    std::optional<std::string> perp_mid{};
    std::optional<std::string> basis_bp{};
    std::optional<std::string> conf{};
    std::optional<std::string> conf_bp{};
    std::optional<std::string> dispersion_bp{};
    std::uint32_t used{0};
    std::uint32_t used_spot{0};
    std::uint32_t used_perp{0};
    std::uint32_t quotes{0};
    std::uint64_t age_ms{0};
    std::uint64_t oldest_age_ms{0};
    Cuts cuts{};
};

struct List {
    bool alive{false};
    std::string published_at{};
    std::uint64_t age_ms{0};
    std::uint64_t period_ms{0};
    std::uint64_t took_us{0};
    std::uint64_t cycles{0};
    std::uint32_t gateways{0};
    std::uint32_t unlisted{0};
    std::uint32_t quotes{0};
    std::uint32_t refused{0};
    std::string usdt_usd{};
    std::string usdc_usd{};
    bool usdt_fallback{true};
    bool usdc_fallback{true};
    std::uint32_t usdt_sources{0};
    std::uint32_t usdc_sources{0};
    std::vector<Item> items{};
};

} // namespace dto

namespace {

constexpr glz::opts k_opts{.skip_null_members = true};
constexpr std::uint64_t k_ms = 1'000'000;

[[nodiscard]] std::optional<std::string> decimal(FixedE9 value) {
    if (value.is_undef()) {
        return std::nullopt;
    }
    char buf[48];
    return std::string{format_e9(value, buf, sizeof buf)};
}

[[nodiscard]] std::optional<std::string> ratio_bp(FixedE9 value) {
    return value.is_undef() ? std::nullopt : std::optional<std::string>{bp_text(value.raw)};
}

[[nodiscard]] dto::Item to_item(const RefCell& cell, std::uint64_t now_ns) {
    const auto cut = [&cell](Cut c) { return std::uint32_t{cell.cuts[static_cast<std::size_t>(c)]}; };
    dto::Item item{.asset = std::string{cell.asset_key()},
                   .refused = (cell.flags & k_flag_refused) != 0,
                   .mid = decimal(cell.mid),
                   .spot_mid = decimal(cell.spot_mid),
                   .perp_mid = decimal(cell.perp_mid),
                   .basis_bp = ratio_bp(cell.basis_e9),
                   .conf = decimal(cell.conf),
                   .dispersion_bp = ratio_bp(cell.dispersion_e9),
                   .used = std::uint32_t{cell.used_spot} + cell.used_perp,
                   .used_spot = cell.used_spot,
                   .used_perp = cell.used_perp,
                   .quotes = cell.quotes,
                   .age_ms = now_ns > cell.ts_ns ? (now_ns - cell.ts_ns) / k_ms : 0,
                   .oldest_age_ms = cell.oldest_age_ns / k_ms,
                   .cuts = {.empty = cut(Cut::Empty),
                            .crossed = cut(Cut::Crossed),
                            .wide = cut(Cut::Wide),
                            .stale = cut(Cut::Stale),
                            .no_rate = cut(Cut::NoRate),
                            .duplicate = cut(Cut::Duplicate),
                            .warming = cut(Cut::Warming),
                            .outlier = cut(Cut::Outlier)}};
    if (!cell.conf.is_undef() && !cell.mid.is_undef() && cell.mid.raw > 0) {
        item.conf_bp = bp_text(mul_div(cell.conf.raw, k_e9, cell.mid.raw));
    }
    return item;
}

// "BTC" matches "crypto:BTC"; a full key matches itself
[[nodiscard]] bool wanted(std::string_view asset, std::string_view filter) {
    if (filter.empty() || asset == filter) {
        return true;
    }
    const auto colon = asset.find(':');
    return colon != std::string_view::npos && asset.substr(colon + 1) == filter;
}

} // namespace

std::string bp_text(std::int64_t ratio_e9) {
    // a hundredth of a basis point is 1'000 in e9
    const auto hundredths = ratio_e9 / 1'000;
    const auto magnitude = hundredths < 0 ? -hundredths : hundredths;
    char buf[32];
    const auto n = std::snprintf(buf,
                                 sizeof buf,
                                 "%s%lld.%02lld",
                                 hundredths < 0 ? "-" : "",
                                 static_cast<long long>(magnitude / 100),
                                 static_cast<long long>(magnitude % 100));
    return std::string{buf, static_cast<std::size_t>(n)};
}

std::optional<http::Response> ReferenceRoute::operator()(const http::Request& req) const {
    if (req.path != k_reference_path) {
        return std::nullopt;
    }
    if (req.method != "GET") {
        return http::problem(405, "method not allowed", "this path serves reads");
    }
    const auto snapshot = feed_->read();
    if (!snapshot.has_value()) {
        return http::problem(503, "no reference published", "refd is not running on this host");
    }
    const auto now = wall_();
    const auto& m = snapshot->metrics;
    char rate[48];
    dto::List list{.alive = snapshot->owner_alive,
                   .published_at = std::to_string(m.published_ns),
                   .age_ms = now > m.published_ns ? (now - m.published_ns) / k_ms : 0,
                   .period_ms = m.period_ns / k_ms,
                   .took_us = m.took_ns / 1'000,
                   .cycles = m.cycles,
                   .gateways = m.gateways,
                   .unlisted = m.unlisted,
                   .quotes = m.quotes,
                   .refused = m.refused,
                   .usdt_usd = std::string{format_e9(m.usdt_usd, rate, sizeof rate)},
                   .usdc_usd = std::string{format_e9(m.usdc_usd, rate, sizeof rate)},
                   .usdt_fallback = (m.flags & k_flag_usdt_fallback) != 0,
                   .usdc_fallback = (m.flags & k_flag_usdc_fallback) != 0,
                   .usdt_sources = m.usdt_sources,
                   .usdc_sources = m.usdc_sources};
    const auto filter = http::query_param(req.query, "asset");
    for (const auto& cell : snapshot->rows) {
        if (wanted(cell.asset_key(), filter)) {
            list.items.push_back(to_item(cell, now));
        }
    }
    std::string out;
    if (glz::write<k_opts>(list, out)) {
        return http::problem(500, "serialisation failed");
    }
    return http::Response{.status = 200, .content_type = "application/json", .body = std::move(out)};
}

} // namespace raikiri::reference
