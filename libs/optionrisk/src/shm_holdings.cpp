#include <raikiri/optionrisk/infra.hpp>

namespace raikiri::optionrisk {

void ShmHoldings::read(std::vector<Holding>& out) const {
    out.clear();
    for (std::size_t i = 0; i < registry_->size(); ++i) {
        const auto* gw = registry_->at(i);
        if (gw == nullptr || gw->channels.catalogue == nullptr || gw->channels.positions == nullptr) {
            continue;
        }
        for (const auto& instrument : gw->channels.catalogue->all()) {
            const auto* option = std::get_if<OptionInstrument>(&instrument);
            if (option == nullptr || option->base.id.symbol().raw >= shm::k_position_cells) {
                continue;
            }
            shm::PositionCell cell{};
            if (gw->channels.positions->read(option->base.id.symbol().raw, cell) == 0 ||
                cell.report.instrument != option->base.id || cell.ts_pulled.ns == 0 || cell.report.net_qty.is_undef() ||
                cell.report.net_qty.raw == 0) {
                continue;
            }
            const auto key = options::key_of(*option);
            if (!key.has_value()) {
                continue;
            }
            const auto entry = cell.report.avg_entry;
            out.push_back(
                {.account = gw->name,
                 .instrument = option->base.id,
                 .symbol = option->base.exchange_symbol,
                 .key = *key,
                 .premium = option->premium,
                 .contracts = option->base.lots_to_qty(cell.report.net_qty),
                 .contract_value = option->base.contract_value,
                 .entry = !entry.is_undef() && entry.raw > 0 ? option->base.ticks_to_price(entry) : FixedE9::undef(),
                 .as_of = cell.ts_pulled});
        }
    }
}

bool ShmHoldings::attached(std::string_view account) const {
    return registry_->find(account) != nullptr;
}

} // namespace raikiri::optionrisk
