#include <raikiri/domain/asset_class.hpp>
#include <raikiri/reference/gateways.hpp>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <map>

namespace raikiri::reference {

namespace {

[[nodiscard]] std::uint64_t file_id_of(const std::string& path) {
    struct stat st {};

    return ::stat(path.c_str(), &st) == 0 ? static_cast<std::uint64_t>(st.st_ino) : 0;
}

// ticks_to_price aborts past i64, and the ticks are the venue's
[[nodiscard]] FixedE9 price_of(const InstrumentBase& base, PriceTicks ticks) noexcept {
    std::int64_t raw = 0;
    if (ticks.is_undef() || base.tick_size.is_undef() || __builtin_mul_overflow(ticks.raw, base.tick_size.raw, &raw)) {
        return FixedE9::undef();
    }
    return FixedE9::from_raw(raw);
}

} // namespace

FixedE9 base_qty(const InstrumentBase& base, QtyLots lots, FixedE9 price) noexcept {
    if (lots.is_undef() || base.lot_size.is_undef() || lots.raw <= 0) {
        return FixedE9::undef();
    }
    std::int64_t qty = 0;
    if (__builtin_mul_overflow(lots.raw, base.lot_size.raw, &qty)) {
        return FixedE9::undef();
    }
    const auto cv = base.contract_value;
    if (base.id.market() == MarketType::Spot || cv.is_undef() || cv.raw <= 0) {
        return FixedE9::from_raw(qty);
    }
    const auto divisor = base.kind == ContractKind::Inverse ? (price.is_undef() ? 0 : price.raw) : k_e9;
    const auto out = divisor <= 0 ? std::nullopt : internal::scaled(qty, cv.raw, divisor);
    return out.has_value() ? FixedE9::from_raw(*out) : FixedE9::undef();
}

std::optional<Quote>
quote_of(const Instrument& listed, const Bbo& bbo, const std::optional<TickerStats>& stats) noexcept {
    const auto& base = base_of(listed);
    const auto market = base.id.market();
    if (market != MarketType::Spot && market != MarketType::Perp) {
        return std::nullopt;
    }
    Quote q{.venue = base.id.venue(),
            .market = market,
            .ccy = quote_ccy_of(base.local_symbol.view()),
            .bid = price_of(base, bbo.bid),
            .ask = price_of(base, bbo.ask),
            .ts_ns = bbo.ts_init.ns};
    q.bid_qty = base_qty(base, bbo.bid_qty, q.bid);
    q.ask_qty = base_qty(base, bbo.ask_qty, q.ask);
    if (stats.has_value()) {
        q.volume_24h = stats->volume_24h_quote;
        if ((q.volume_24h.is_undef() || q.volume_24h.raw <= 0) && !q.bid.is_undef() && !q.ask.is_undef()) {
            q.volume_24h = turnover_e6(stats->volume_24h_base, FixedE9::from_raw(q.bid.raw / 2 + q.ask.raw / 2));
        }
    }
    return q;
}

std::size_t GatewayQuotes::unlisted() const noexcept {
    return static_cast<std::size_t>(
        std::ranges::count_if(sources_, [](const auto& s) { return !s->client.reader_listed(); }));
}

GatewayQuotes::Rescan GatewayQuotes::rescan() {
    Rescan out;
    std::map<std::string, std::uint64_t> present;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator{shm_dir_, ec}) {
        const auto name = entry.path().filename().string();
        if (name.starts_with("gw.")) {
            present.emplace(name.substr(3), file_id_of(entry.path().string()));
        }
    }
    if (ec) {
        // an unreadable directory says nothing about the gateways, so nothing is detached
        return out;
    }
    std::erase_if(sources_, [&](const std::unique_ptr<Source>& src) {
        const auto found = present.find(src->name);
        if (found != present.end() && found->second == src->file_id) {
            return false;
        }
        out.gone.push_back(src->name);
        return true;
    });
    for (const auto& [name, file_id] : present) {
        if (file_id == 0 || std::ranges::any_of(sources_, [&](const auto& s) { return s->name == name; })) {
            continue;
        }
        if (account_from_string(name).venue == VenueId::Invalid) {
            continue;
        }
        const auto path = shm_dir_ + "/gw." + name;
        auto client = shm::SegmentClient::attach(path);
        // not ready yet, or replaced between the listing and the map: the next rescan tries again
        if (!client.has_value() || file_id_of(path) != file_id) {
            continue;
        }
        auto channels = shm::open_gateway_channels(*client);
        if (!channels.has_value()) {
            continue;
        }
        if (!client->register_reader(::getpid(), reader_name_).has_value()) {
            out.unlisted.push_back(name);
        }
        sources_.push_back(std::make_unique<Source>(
            Source{.name = name, .client = std::move(*client), .channels = *channels, .file_id = file_id}));
        out.attached.push_back(name);
    }
    return out;
}

std::vector<AssetQuotes> GatewayQuotes::snapshot() {
    std::map<std::string, std::vector<Quote>, std::less<>> by_asset;
    const auto mono = mono_();
    for (const auto& src : sources_) {
        // a dead gateway's cells are last-known prices, not current ones
        if (!src->client.owner_alive(mono)) {
            continue;
        }
        const auto& ch = src->channels;
        for (const auto& listed : ch.catalogue->all()) {
            const auto& base = base_of(listed);
            const auto slot = base.id.symbol().raw;
            if (slot >= shm::k_bbo_cells) {
                continue;
            }
            Bbo bbo{};
            if (ch.bbo->read(slot, bbo) == 0) {
                continue;
            }
            auto asset = asset_key(base);
            if (asset.empty()) {
                continue;
            }
            std::optional<TickerStats> stats;
            if (TickerStats got{}; ch.ticker_stats->read(slot, got) != 0) {
                stats = got;
            }
            if (const auto q = quote_of(listed, bbo, stats); q.has_value()) {
                by_asset[std::move(asset)].push_back(*q);
            }
        }
    }
    std::vector<AssetQuotes> out;
    out.reserve(by_asset.size());
    for (auto& [asset, quotes] : by_asset) {
        out.push_back({.asset = asset, .quotes = std::move(quotes)});
    }
    return out;
}

} // namespace raikiri::reference
