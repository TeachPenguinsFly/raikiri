#include <raikiri/optiondata/shm_market.hpp>

namespace raikiri::optiondata {

void ShmOptionMarket::refresh() const {
    std::uint64_t listed = 0;
    for (std::size_t i = 0; i < registry_->size(); ++i) {
        if (const auto* gw = registry_->at(i); gw != nullptr && gw->channels.catalogue != nullptr) {
            listed += gw->channels.catalogue->size();
        }
    }
    if (registry_->epoch() == epoch_ && registry_->size() == gateways_ && listed == listed_) {
        return;
    }
    epoch_ = registry_->epoch();
    gateways_ = registry_->size();
    listed_ = listed;
    index_.clear();
    std::vector<ListedOption> all;
    for (std::size_t i = 0; i < registry_->size(); ++i) {
        const auto* gw = registry_->at(i);
        if (gw == nullptr || gw->channels.catalogue == nullptr) {
            continue;
        }
        for (const auto& instrument : gw->channels.catalogue->all()) {
            const auto* option = std::get_if<OptionInstrument>(&instrument);
            if (option == nullptr || option->base.id.symbol().raw >= shm::k_bbo_cells) {
                continue;
            }
            const auto key = options::key_of(*option);
            if (!key.has_value()) {
                continue;
            }
            index_[std::string{key->underlying.view()}][key->expiry.ns].push_back(
                {.channels = &gw->channels, .option = option, .key = *key});
            all.push_back({.venue = option->base.id.venue(), .key = *key});
        }
    }
    summary_ = summarise(all);
}

std::vector<UnderlyingListing> ShmOptionMarket::underlyings() const {
    refresh();
    return summary_;
}

void ShmOptionMarket::read(std::string_view underlying,
                           std::optional<UnixNanos> expiry,
                           std::vector<OptionQuote>& out) const {
    out.clear();
    refresh();
    const auto found = index_.find(underlying);
    if (found == index_.end()) {
        return;
    }
    const auto take = [&out](const std::vector<Listed>& listed) {
        for (const auto& l : listed) {
            const auto& base = l.option->base;
            const auto cell = base.id.symbol().raw;
            OptionQuote q{.instrument = base.id,
                          .key = l.key,
                          .symbol = base.exchange_symbol,
                          .premium = l.option->premium,
                          .greeks_unit = l.option->greeks};
            Bbo bbo{};
            if (l.channels->bbo != nullptr && l.channels->bbo->read(cell, bbo) != 0 && bbo.instrument == base.id) {
                if (!bbo.bid.is_undef()) {
                    q.bid = base.ticks_to_price(bbo.bid);
                    q.bid_size = base.lots_to_qty(bbo.bid_qty);
                }
                if (!bbo.ask.is_undef()) {
                    q.ask = base.ticks_to_price(bbo.ask);
                    q.ask_size = base.lots_to_qty(bbo.ask_qty);
                }
                q.ts = bbo.ts_event;
            }
            OptionGreeks greeks{};
            if (l.channels->greeks != nullptr && l.channels->greeks->read(cell, greeks) != 0 &&
                greeks.instrument == base.id) {
                q.greeks = greeks;
            }
            TickerStats ticker{};
            if (l.channels->ticker_stats != nullptr && l.channels->ticker_stats->read(cell, ticker) != 0 &&
                ticker.instrument == base.id) {
                q.open_interest = ticker.open_interest;
                q.volume_24h = ticker.volume_24h_base;
            }
            out.push_back(q);
        }
    };
    if (expiry.has_value()) {
        if (const auto e = found->second.find(expiry->ns); e != found->second.end()) {
            take(e->second);
        }
        return;
    }
    for (const auto& [ns, listed] : found->second) {
        take(listed);
    }
}

} // namespace raikiri::optiondata
