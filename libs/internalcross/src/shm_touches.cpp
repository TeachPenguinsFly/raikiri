#include <raikiri/exec/caps.hpp>
#include <raikiri/internalcross/infrastructure/shm_touches.hpp>

#include <algorithm>

namespace raikiri::internalcross {

namespace internal {

constexpr std::uint64_t k_relist_ns = 60ULL * 1'000'000'000ULL;

[[nodiscard]] bool takes_orders(VenueId venue, MarketType market) noexcept {
    const auto& caps = exec::caps_of(venue);
    return caps.serves(market) && !caps.manual_only.has(market) && (caps.ws_order_entry || caps.fix_order_entry);
}

} // namespace internal

ShmTouches::ShmTouches(std::vector<TouchGateway> gateways, const Clock& clock)
    : gateways_{std::move(gateways)}, clock_{&clock} {}

void ShmTouches::relist(InstrumentId instrument, Found& found) const {
    found.siblings.clear();
    const InstrumentBase* own = nullptr;
    for (const auto& gw : gateways_) {
        if (gw.account.venue == instrument.venue() && gw.account.market == instrument.market() &&
            gw.channels.catalogue != nullptr) {
            if (const auto* listed = gw.channels.catalogue->find(instrument); listed != nullptr) {
                own = &base_of(*listed);
                found.siblings.push_back({.gateway = &gw, .id = instrument, .tick = own->tick_size});
            }
        }
    }
    // an option's name means nothing across venues, so its only touch is its own
    if (own == nullptr || instrument.market() == MarketType::Option) {
        return;
    }
    for (const auto& gw : gateways_) {
        if (gw.account.market != instrument.market() || gw.account.venue == instrument.venue() ||
            gw.channels.catalogue == nullptr || !internal::takes_orders(gw.account.venue, gw.account.market)) {
            continue;
        }
        for (const auto& listed : gw.channels.catalogue->all()) {
            const auto& base = base_of(listed);
            if (base.local_symbol == own->local_symbol && base.kind == own->kind) {
                found.siblings.push_back({.gateway = &gw, .id = base.id, .tick = base.tick_size});
                break;
            }
        }
    }
}

std::size_t ShmTouches::read(InstrumentId instrument, std::span<Touch> out) {
    auto& found = listed_[instrument.packed];
    const auto now = clock_->mono().ns;
    if (found.at == 0 || now - found.at >= internal::k_relist_ns) {
        relist(instrument, found);
        found.at = now;
    }
    std::size_t count = 0;
    for (const auto& sibling : found.siblings) {
        if (count == out.size()) {
            break;
        }
        const auto cell = sibling.id.symbol().raw;
        Bbo bbo{};
        Touch touch{.own = sibling.id == instrument};
        if (sibling.gateway->channels.bbo != nullptr && cell < shm::k_bbo_cells &&
            sibling.gateway->channels.bbo->read(cell, bbo) != 0 && !bbo.bid.is_undef() && !bbo.ask.is_undef() &&
            !sibling.tick.is_undef() && sibling.tick.raw > 0) {
            touch.bid = sibling.tick.scaled_by(bbo.bid.raw);
            touch.ask = sibling.tick.scaled_by(bbo.ask.raw);
            touch.ts = bbo.ts_init;
        }
        out[count++] = touch;
    }
    return count;
}

} // namespace raikiri::internalcross
