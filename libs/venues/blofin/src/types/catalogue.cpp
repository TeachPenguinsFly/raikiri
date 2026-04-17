#include <raikiri/venues/blofin/catalogue.hpp>
#include <raikiri/venues/blofin/envelope.hpp>
#include <raikiri/wire/json.hpp>

#include <utility>

namespace raikiri::venues::blofin {

std::expected<std::span<const WireInstrument>, WireError>
parse_instruments(std::string_view body, InstrumentBuffer& storage) {
    return read_data<WireInstrument>(
        body,
        storage.rows,
        [](wire::ondemand::object& obj, WireInstrument& out) -> std::expected<void, WireError> {
            const auto inst_id = wire::req_str(obj, "instId");
            if (!inst_id.has_value()) {
                return std::unexpected(inst_id.error());
            }
            out.inst_id = *inst_id;
            for (const auto& [key, field] : {std::pair{"instType", &out.inst_type},
                                             std::pair{"state", &out.state},
                                             std::pair{"baseCurrency", &out.base_ccy},
                                             std::pair{"quoteCurrency", &out.quote_ccy},
                                             std::pair{"settleCurrency", &out.settle_ccy},
                                             std::pair{"contractType", &out.contract_type},
                                             std::pair{"contractValue", &out.contract_value},
                                             std::pair{"tickSize", &out.tick_size},
                                             std::pair{"lotSize", &out.lot_size}}) {
                const auto value = wire::opt_str(obj, key);
                if (!value.has_value()) {
                    return std::unexpected(value.error());
                }
                *field = *value;
            }
            return {};
        });
}

} // namespace raikiri::venues::blofin
