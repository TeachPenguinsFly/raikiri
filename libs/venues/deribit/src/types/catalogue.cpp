#include <raikiri/venues/deribit/catalogue.hpp>
#include <raikiri/venues/deribit/envelope.hpp>
#include <raikiri/wire/json.hpp>

#include <utility>

namespace raikiri::venues::deribit {

namespace {

using wire::guarded;
using wire::session;

} // namespace

std::expected<std::span<const WireInstrument>, WireError> parse_instruments(std::string_view body,
                                                                            InstrumentBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireInstrument>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = wire::req_array(*root, "result");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_instruments) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WireInstrument out{};
            const auto name = wire::req_str(obj, "instrument_name");
            const auto kind = wire::req_str(obj, "kind");
            if (!name.has_value() || !kind.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.instrument_name = *name;
            out.kind = *kind;
            for (const auto& [key, field] : {std::pair{"instrument_type", &out.instrument_type},
                                             std::pair{"settlement_period", &out.settlement_period},
                                             std::pair{"option_type", &out.option_type},
                                             std::pair{"base_currency", &out.base_currency},
                                             std::pair{"counter_currency", &out.counter_currency},
                                             std::pair{"underlying_type", &out.underlying_type}}) {
                const auto value = wire::opt_str(obj, key);
                if (!value.has_value()) {
                    return std::unexpected(value.error());
                }
                *field = *value;
            }
            for (const auto& [key, field] : {std::pair{"tick_size", &out.tick_size},
                                             std::pair{"contract_size", &out.contract_size},
                                             std::pair{"min_trade_amount", &out.min_trade_amount},
                                             std::pair{"strike", &out.strike}}) {
                const auto value = opt_num(obj, key);
                if (!value.has_value()) {
                    return std::unexpected(value.error());
                }
                *field = *value;
            }
            const auto expiry = opt_ms(obj, "expiration_timestamp");
            const auto active = wire::opt_bool(obj, "is_active");
            if (!expiry.has_value() || !active.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.expiration_ms = *expiry;
            out.is_active = *active;
            storage.instruments[count] = out;
            ++count;
        }
        return std::span<const WireInstrument>{storage.instruments.data(), count};
    });
}

} // namespace raikiri::venues::deribit
