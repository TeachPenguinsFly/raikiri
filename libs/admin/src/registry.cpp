#include <raikiri/admin/format.hpp>
#include <raikiri/admin/registry.hpp>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <variant>

namespace raikiri::admin {

namespace {

constexpr std::string_view k_segment_prefix = "gw.";

} // namespace

Instrument describe(const raikiri::Instrument& instrument, std::string_view account) {
    const auto& base = base_of(instrument);
    Instrument out{};
    out.instrument_id = digits(base.id);
    out.account = std::string{account};
    out.venue = std::string{to_string(base.id.venue())};
    out.market = std::string{to_string(base.id.market())};
    out.exchange_symbol = std::string{base.exchange_symbol.view()};
    out.local_symbol = std::string{base.local_symbol.view()};
    out.tick_size = decimal(base.tick_size);
    out.lot_size = decimal(base.lot_size);
    out.min_notional = decimal(base.min_notional);
    out.contract_value = decimal(base.contract_value);
    out.contract_kind = std::string{contract_kind_name(base.kind)};

    if (const auto* future = std::get_if<FutureInstrument>(&instrument); future != nullptr) {
        out.expiry = digits(future->expiry);
    } else if (const auto* option = std::get_if<OptionInstrument>(&instrument); option != nullptr) {
        out.expiry = digits(option->expiry);
        out.strike = price(option->strike, base);
        out.put_call = option->put_call == PutCall::Put ? "put" : "call";
    }
    return out;
}

std::size_t Registry::discover(const std::string& shm_dir, std::int32_t reader_pid) {
    std::error_code ec;
    std::filesystem::directory_iterator it{shm_dir, ec};
    if (ec) {
        return 0; // no runtime directory means no gateways, not an error
    }
    for (const auto& entry : it) {
        const auto name = entry.path().filename().string();
        if (!name.starts_with(k_segment_prefix)) {
            continue;
        }
        const auto account_name = name.substr(k_segment_prefix.size());
        const auto account = account_from_string(account_name);
        if (account.venue == VenueId::Invalid) {
            continue; // a file whose name is not an account is not ours
        }
        auto* held = find_mutable(account_name);
        auto client = shm::SegmentClient::attach(entry.path().string());
        if (!client.has_value()) {
            continue; // wrong abi, wrong layout or dead owner: skip
        }
        // same generation is the same run; a moved generation is a restarted gateway
        if (held != nullptr && held->client.generation() == client->generation()) {
            continue;
        }
        auto channels = shm::open_gateway_channels(*client);
        if (!channels.has_value()) {
            // the file exists before the channels are published; a later rescan catches it
            continue;
        }
        (void)client->register_reader(reader_pid);
        Gateway fresh{.account = account, .name = account_name, .client = std::move(*client), .channels = *channels};
        if (held != nullptr) {
            // in place so the gateway keeps its address; pointers into the old mapping now dangle, hence the epoch
            *held = std::move(fresh);
            ++epoch_;
            continue; // the set of gateways did not change, so neither did its order
        }
        gateways_.push_back(std::make_unique<Gateway>(std::move(fresh)));
    }
    std::ranges::sort(gateways_, {}, [](const auto& gw) { return gw->name; });
    return gateways_.size();
}

namespace {

// convert the monotonic heartbeat by its age; zero stays zero
[[nodiscard]] std::uint64_t heartbeat_wall(const Gateway& gw, MonoNanos now, UnixNanos wall) {
    const auto beat = gw.client.last_heartbeat();
    if (beat == 0 || beat > now.ns) {
        return 0; // never stamped, or stamped ahead of us
    }
    const auto age = now.ns - beat;
    return age > wall.ns ? 0 : wall.ns - age;
}

} // namespace

// cells written at least once since the segment was created; cumulative so quiet symbols still count
[[nodiscard]] std::int64_t priced_count(const shm::GatewayChannels& ch) noexcept {
    const auto listed = std::min<std::size_t>(ch.catalogue->size(), shm::k_bbo_cells);
    std::int64_t priced = 0;
    for (std::size_t i = 0; i < listed; ++i) {
        Bbo bbo{};
        if (ch.bbo->read(i, bbo) != 0 && bbo.ts_init.ns != 0) {
            ++priced;
        }
    }
    return priced;
}

std::vector<Account> Registry::accounts(MonoNanos now, UnixNanos wall) const {
    std::vector<Account> out;
    out.reserve(gateways_.size());
    for (const auto& owned : gateways_) {
        const auto& gw = *owned;
        const bool alive = gw.client.owner_alive(now.ns);
        const auto listed = static_cast<std::int64_t>(gw.channels.catalogue->size());
        const auto priced = priced_count(gw.channels);
        out.push_back({.account = gw.name,
                       .venue = std::string{to_string(gw.account.venue)},
                       .market = std::string{to_string(gw.account.market)},
                       .gateway = {.alive = alive,
                                   .generation = digits(gw.client.generation()),
                                   .last_heartbeat = digits(heartbeat_wall(gw, now, wall)),
                                   .instruments = listed,
                                   .priced = priced,
                                   .coverage_pct = listed > 0 ? priced * 100 / listed : 0,
                                   .subscribe_refusals = static_cast<std::int64_t>(gw.client.subscribe_refusals())}});
    }
    return out;
}

Health Registry::health(MonoNanos now, UnixNanos wall) const {
    // the database half is filled by the caller
    return {.gateways = accounts(now, wall), .database = {}};
}

std::vector<Instrument> Registry::instruments(std::string_view account, std::optional<MarketType> market) const {
    std::vector<Instrument> out;
    for (const auto& owned : gateways_) {
        const auto& gw = *owned;
        if (!account.empty() && gw.name != account) {
            continue;
        }
        if (market.has_value() && gw.account.market != *market) {
            continue;
        }
        for (const auto& listed : gw.channels.catalogue->all()) {
            out.push_back(describe(listed, gw.name));
        }
    }
    return out;
}

namespace {

// case-insensitive substring without allocating
[[nodiscard]] bool contains_fold(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > haystack.size()) {
        return false;
    }
    const auto up = [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; };
    for (std::size_t start = 0; start + needle.size() <= haystack.size(); ++start) {
        std::size_t i = 0;
        while (i < needle.size() && up(haystack[start + i]) == up(needle[i])) {
            ++i;
        }
        if (i == needle.size()) {
            return true;
        }
    }
    return false;
}

} // namespace

std::vector<Market> Registry::markets(std::string_view query, std::size_t limit) const {
    std::unordered_map<std::string, Market> by_symbol;
    for (const auto& owned : gateways_) {
        const auto& gw = *owned;
        for (const auto& listed : gw.channels.catalogue->all()) {
            const auto& base = base_of(listed);
            const auto symbol = std::string{base.local_symbol.view()};
            if (symbol.empty() || !contains_fold(symbol, query)) {
                continue;
            }
            auto& market = by_symbol[symbol];
            if (market.symbol.empty()) {
                market.symbol = symbol;
            }
            market.listings.push_back({.instrument_id = digits(base.id.packed),
                                       .account = gw.name,
                                       .venue = std::string{to_string(gw.account.venue)},
                                       .market = std::string{to_string(gw.account.market)},
                                       .exchange_symbol = std::string{base.exchange_symbol.view()}});
        }
    }

    std::vector<Market> out;
    out.reserve(by_symbol.size());
    for (auto& [symbol, market] : by_symbol) {
        std::ranges::sort(market.listings, {}, &Listing::venue);
        out.push_back(std::move(market));
    }
    std::ranges::sort(out, [](const Market& a, const Market& b) {
        if (a.listings.size() != b.listings.size()) {
            return a.listings.size() > b.listings.size();
        }
        return a.symbol < b.symbol;
    });
    if (out.size() > limit) {
        out.resize(limit);
    }
    return out;
}

std::optional<Registry::CatalogueRow> Registry::catalogue_row(InstrumentId id) const noexcept {
    for (const auto& owned : gateways_) {
        const auto& gw = *owned;
        if (gw.account.venue != id.venue() || gw.account.market != id.market()) {
            continue;
        }
        if (const auto* listed = gw.channels.catalogue->find(id); listed != nullptr) {
            return CatalogueRow{.gateway = &gw, .instrument = listed};
        }
    }
    return std::nullopt;
}

std::optional<Instrument> Registry::instrument(InstrumentId id) const {
    for (const auto& owned : gateways_) {
        const auto& gw = *owned;
        // the (venue, market) prefix picks the one catalogue that can hold it
        if (gw.account.venue != id.venue() || gw.account.market != id.market()) {
            continue;
        }
        if (const auto* listed = gw.channels.catalogue->find(id); listed != nullptr) {
            return describe(*listed, gw.name);
        }
    }
    return std::nullopt;
}

std::vector<AccountBalances> Registry::balances(std::string_view account, UnixNanos now) const {
    // three missed 30 s pulls, same window as the live positions feed
    constexpr std::uint64_t k_stale_ns = 90ULL * 1'000'000'000ULL;

    std::vector<AccountBalances> out;
    for (const auto& owned : gateways_) {
        const auto& gw = *owned;
        if (!account.empty() && gw.name != account) {
            continue;
        }
        AccountBalances entry{.account = gw.name,
                              .venue = std::string{to_string(gw.account.venue)},
                              .reported_at = std::nullopt,
                              .stale = false,
                              .assets = {}};
        if (gw.channels.balances == nullptr) {
            out.push_back(std::move(entry));
            continue;
        }
        // one read of the whole set so assets come from one snapshot
        shm::BalanceSnapshot snapshot{};
        // version 0 is a never-written cell
        const auto version = gw.channels.balances->read(0, snapshot);
        if (version == 0 || snapshot.ts_pulled.ns == 0) {
            // not answered yet: empty with no timestamp
            out.push_back(std::move(entry));
            continue;
        }
        entry.reported_at = digits(snapshot.ts_pulled.ns);
        entry.stale = now.ns > snapshot.ts_pulled.ns && (now.ns - snapshot.ts_pulled.ns) > k_stale_ns;
        const auto rows = std::min<std::size_t>(snapshot.count, shm::k_balance_rows);
        entry.assets.reserve(rows);
        for (std::size_t i = 0; i < rows; ++i) {
            const auto& row = snapshot.rows.at(i);
            if (!row.has_asset()) {
                continue;
            }
            Balance balance{.asset = std::string{row.asset.view()},
                            .total = std::nullopt,
                            .available = std::nullopt,
                            .unrealized = std::nullopt};
            // undef stays absent
            if (!row.total.is_undef()) {
                balance.total = decimal(row.total);
            }
            if (!row.available.is_undef()) {
                balance.available = decimal(row.available);
            }
            if (!row.unrealized.is_undef()) {
                balance.unrealized = decimal(row.unrealized);
            }
            entry.assets.push_back(std::move(balance));
        }
        out.push_back(std::move(entry));
    }
    return out;
}

const Gateway* Registry::find(std::string_view account) const noexcept {
    const auto it = std::ranges::find(gateways_, account, [](const auto& gw) { return gw->name; });
    return it == gateways_.end() ? nullptr : it->get();
}

Gateway* Registry::find_mutable(std::string_view account) noexcept {
    const auto it = std::ranges::find(gateways_, account, [](const auto& gw) { return gw->name; });
    return it == gateways_.end() ? nullptr : it->get();
}

} // namespace raikiri::admin
