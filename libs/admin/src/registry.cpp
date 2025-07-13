#include <raikiri/admin/registry.hpp>

#include <raikiri/admin/format.hpp>

#include <algorithm>
#include <filesystem>
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

    // Absent rather than zero on the instruments that have none: an expiry of
    // zero is a date in 1970 and a strike of zero is a free option, and both
    // would be read as data.
    if (const auto* future = std::get_if<FutureInstrument>(&instrument); future != nullptr) {
        out.expiry = digits(future->expiry);
    } else if (const auto* option = std::get_if<OptionInstrument>(&instrument);
               option != nullptr) {
        out.expiry = digits(option->expiry);
        // Through the instrument, because a strike is a price and a tick count
        // without its instrument is not one.
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
        auto client = shm::SegmentClient::attach(entry.path().string());
        if (!client.has_value()) {
            continue; // wrong ABI, wrong layout, dead owner: skip, never fatal
        }
        auto channels = shm::open_gateway_channels(*client);
        if (!channels.has_value()) {
            continue;
        }
        (void)client->register_reader(reader_pid);
        gateways_.push_back(
            {.account = account,
             .name = account_name,
             .client = std::move(*client),
             .channels = *channels});
    }
    std::ranges::sort(gateways_, {}, &Gateway::name);
    return gateways_.size();
}

std::vector<Account> Registry::accounts(MonoNanos now) const {
    std::vector<Account> out;
    out.reserve(gateways_.size());
    for (const auto& gw : gateways_) {
        // Monotonic, because that is the clock the gateway stamps its
        // heartbeat with; asking in wall time makes every owner look stale.
        const bool alive = gw.client.owner_alive(now.ns);
        out.push_back({.account = gw.name,
                       .venue = std::string{to_string(gw.account.venue)},
                       .market = std::string{to_string(gw.account.market)},
                       .gateway = {.alive = alive,
                                   .generation = digits(gw.client.generation()),
                                   .last_heartbeat = digits(gw.client.last_heartbeat()),
                                   .instruments = gw.channels.catalogue->size()}});
    }
    return out;
}

Health Registry::health(MonoNanos now) const {
    // The database half is filled by the caller, which owns the connection.
    return {.gateways = accounts(now), .database = {}};
}

std::vector<Instrument> Registry::instruments(std::string_view account,
                                              std::optional<MarketType> market) const {
    std::vector<Instrument> out;
    for (const auto& gw : gateways_) {
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

std::optional<Instrument> Registry::instrument(InstrumentId id) const {
    for (const auto& gw : gateways_) {
        // The (venue, market) prefix says which gateway can possibly hold it,
        // so this asks one catalogue rather than every one.
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
    // Three missed pulls at the gateway's 30 s cadence, the same window the
    // live positions feed uses. One late round is not a venue going quiet.
    constexpr std::uint64_t k_stale_ns = 90ULL * 1'000'000'000ULL;

    std::vector<AccountBalances> out;
    for (const auto& gw : gateways_) {
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
        // ONE read of the whole set. A row-at-a-time reader could take two
        // assets from one snapshot and two from the next, and sum them into an
        // equity the account never had at any instant.
        shm::BalanceSnapshot snapshot{};
        // Version 0 is a cell nothing has written, which is absence rather than
        // a value — the same rule the change gate on the live stream uses.
        const auto version = gw.channels.balances->read(0, snapshot);
        if (version == 0 || snapshot.ts_pulled.ns == 0) {
            // Never written: the venue has not answered yet, or cannot. Empty
            // with no timestamp, which is not the same as holding nothing.
            out.push_back(std::move(entry));
            continue;
        }
        entry.reported_at = digits(snapshot.ts_pulled.ns);
        entry.stale =
            now.ns > snapshot.ts_pulled.ns && (now.ns - snapshot.ts_pulled.ns) > k_stale_ns;
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
            // Undef stays ABSENT rather than becoming "0": the contract's
            // Decimal has no spelling for "the venue did not say".
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
    const auto it = std::ranges::find(gateways_, account, &Gateway::name);
    return it == gateways_.end() ? nullptr : &*it;
}

} // namespace raikiri::admin
