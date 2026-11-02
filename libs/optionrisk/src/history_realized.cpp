#include <raikiri/optionrisk/infra.hpp>

#include <charconv>
#include <map>

namespace raikiri::optionrisk {

void HistoryRealized::read(std::vector<BookedFill>& out) const {
    out.clear();
    std::map<std::uint64_t, BookedFill> rows;
    for (const auto& position : history_->positions(std::nullopt, std::nullopt)) {
        std::uint64_t packed = 0;
        const auto& id = position.instrument_id;
        if (std::from_chars(id.data(), id.data() + id.size(), packed).ec != std::errc{}) {
            continue;
        }
        const InstrumentId instrument{packed};
        if (instrument.market() != MarketType::Option) {
            continue;
        }
        const auto listed = registry_->catalogue_row(instrument);
        if (!listed.has_value()) {
            continue;
        }
        const auto* option = std::get_if<OptionInstrument>(listed->instrument);
        const auto key = option != nullptr ? options::key_of(*option) : std::nullopt;
        const auto realized = parse_decimal_e9(position.realized);
        if (!key.has_value() || !realized.has_value()) {
            continue;
        }
        auto [row, fresh] = rows.try_emplace(
            packed,
            BookedFill{.instrument = instrument, .key = *key, .premium = option->premium, .realized = *realized});
        if (!fresh) {
            row->second.realized = row->second.realized + *realized;
        }
    }
    out.reserve(rows.size());
    for (const auto& [packed, row] : rows) {
        out.push_back(row);
    }
}

} // namespace raikiri::optionrisk
