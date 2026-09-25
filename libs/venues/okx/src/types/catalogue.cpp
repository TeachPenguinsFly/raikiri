#include <raikiri/venues/okx/catalogue.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::okx {

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
        auto array = wire::req_array(*root, "data");
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
            const auto inst_id = wire::req_str(obj, "instId");
            const auto inst_type = wire::req_str(obj, "instType");
            if (!inst_id.has_value() || !inst_type.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.inst_id = *inst_id;
            out.inst_type = *inst_type;
            for (const auto& [key, field] : {std::pair{"state", &out.state},
                                             std::pair{"baseCcy", &out.base_ccy},
                                             std::pair{"quoteCcy", &out.quote_ccy},
                                             std::pair{"tickSz", &out.tick_size},
                                             std::pair{"lotSz", &out.lot_size},
                                             std::pair{"minSz", &out.min_size},
                                             std::pair{"ctVal", &out.ct_val},
                                             std::pair{"ctMult", &out.ct_mult},
                                             std::pair{"ctType", &out.ct_type},
                                             std::pair{"instCategory", &out.inst_category}}) {
                const auto value = wire::opt_str(obj, key);
                if (!value.has_value()) {
                    return std::unexpected(value.error());
                }
                *field = *value;
            }
            storage.instruments[count] = out;
            ++count;
        }
        return std::span<const WireInstrument>{storage.instruments.data(), count};
    });
}

} // namespace raikiri::venues::okx
