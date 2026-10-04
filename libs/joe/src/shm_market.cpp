#include <raikiri/exec/caps.hpp>
#include <raikiri/joe/shm_market.hpp>

#include <utility>

namespace raikiri::joe {
namespace internal {

constexpr std::uint64_t k_relist_ns = 60ULL * 1'000'000'000ULL;
// a book older than this is not routed to
constexpr std::uint64_t k_stale_ns = 5ULL * 1'000'000'000ULL;

[[nodiscard]] bool takes_orders(VenueId venue, MarketType market) noexcept {
    const auto& caps = exec::caps_of(venue);
    return caps.serves(market) && !caps.manual_only.has(market) && (caps.ws_order_entry || caps.fix_order_entry);
}

[[nodiscard]] FixedE9 lot_of(const InstrumentBase& base) noexcept {
    return value_of(base.lot_size, base.contract_value);
}

[[nodiscard]] FixedE9 or_zero(FixedE9 value) noexcept {
    return value.is_undef() ? FixedE9::from_raw(0) : value;
}

} // namespace internal

ShmMarket::ShmMarket(std::vector<GatewayView> gateways, const Clock& clock)
    : gateways_{std::move(gateways)}, clock_{&clock} {}

const GatewayView* ShmMarket::gateway_for(InstrumentId id) const noexcept {
    for (const auto& gw : gateways_) {
        if (gw.account.venue == id.venue() && gw.account.market == id.market()) {
            return &gw;
        }
    }
    return nullptr;
}

const std::vector<InstrumentId>& ShmMarket::listed(const Listing& listing) const {
    auto key = std::string{listing.symbol.view()} + (listing.market == MarketType::Spot ? ":spot" : ":perp");
    const auto now = clock_->mono().ns;
    auto& found = listed_[std::move(key)];
    if (found.at != 0 && now - found.at < internal::k_relist_ns) {
        return found.ids;
    }
    found.ids.clear();
    found.at = now;
    for (const auto& gw : gateways_) {
        if (gw.channels.catalogue == nullptr || gw.account.market != listing.market ||
            !internal::takes_orders(gw.account.venue, gw.account.market)) {
            continue;
        }
        for (const auto& instrument : gw.channels.catalogue->all()) {
            const auto& base = base_of(instrument);
            if (base.local_symbol == listing.symbol && base.kind == ContractKind::Linear &&
                base.id.market() == listing.market) {
                found.ids.push_back(base.id);
            }
        }
    }
    return found.ids;
}

std::vector<VenueBook> ShmMarket::books(const Listing& listing, Side side) const {
    std::vector<VenueBook> out;
    const auto wall = clock_->wall().ns;
    for (const auto id : listed(listing)) {
        const auto* gw = gateway_for(id);
        const auto* instrument = gw != nullptr ? gw->channels.catalogue->find(id) : nullptr;
        if (instrument == nullptr) {
            continue;
        }
        const auto& base = base_of(*instrument);
        VenueBook book{.instrument = id,
                       .taker_fee = internal::or_zero(base.taker_fee),
                       .lot = internal::lot_of(base),
                       .min_notional = internal::or_zero(base.min_notional)};
        if (book.lot.raw <= 0 || base.tick_size.is_undef() || base.tick_size.raw <= 0) {
            continue;
        }
        const auto cell = id.symbol().raw;
        const auto add = [&](PriceTicks price, QtyLots qty) {
            if (!price.is_undef() && !qty.is_undef() && price.raw > 0 && qty.raw > 0) {
                book.levels.push_back(
                    {.price = base.tick_size.scaled_by(price.raw), .qty = book.lot.scaled_by(qty.raw)});
            }
        };
        BookSnapshot snapshot{};
        if (gw->channels.depth != nullptr && cell < shm::k_depth_cells &&
            gw->channels.depth->read(cell, snapshot) != 0 && snapshot.ts_init.ns + internal::k_stale_ns > wall) {
            const auto& levels = side == Side::Buy ? snapshot.asks : snapshot.bids;
            const auto count = std::min<std::size_t>(side == Side::Buy ? snapshot.ask_count : snapshot.bid_count,
                                                     levels.size());
            for (std::size_t i = 0; i < count; ++i) {
                add(levels[i].price, levels[i].qty);
            }
        }
        Bbo bbo{};
        if (book.levels.empty() && gw->channels.bbo != nullptr && cell < shm::k_bbo_cells &&
            gw->channels.bbo->read(cell, bbo) != 0 && bbo.ts_init.ns + internal::k_stale_ns > wall) {
            add(side == Side::Buy ? bbo.ask : bbo.bid, side == Side::Buy ? bbo.ask_qty : bbo.bid_qty);
        }
        if (!book.levels.empty()) {
            out.push_back(std::move(book));
        }
    }
    return out;
}

std::optional<Scale> ShmMarket::scale(InstrumentId instrument) const {
    const auto* gw = gateway_for(instrument);
    const auto* found = gw != nullptr && gw->channels.catalogue != nullptr ? gw->channels.catalogue->find(instrument)
                                                                           : nullptr;
    if (found == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*found);
    return Scale{.listing = {.symbol = base.local_symbol, .market = instrument.market()},
                 .tick = base.tick_size,
                 .lot = internal::lot_of(base)};
}

} // namespace raikiri::joe
