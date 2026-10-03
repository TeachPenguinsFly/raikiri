#include <raikiri/venues/okx/catalogue.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::okx {

namespace {

using wire::guarded;
using wire::session;

} // namespace

std::expected<std::span<const std::string_view>, WireError> parse_underlyings(std::string_view body,
                                                                              UnderlyingBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const std::string_view>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = wire::req_array(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        std::size_t count = 0;
        for (auto group : *data) {
            wire::ondemand::array names;
            if (const auto ec = group.get_array().get(names); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            for (auto name : names) {
                if (count == k_max_underlyings) {
                    return std::unexpected(WireError::TooManyEntries);
                }
                std::string_view text;
                if (const auto ec = name.get_string().get(text); ec != simdjson::SUCCESS) {
                    return std::unexpected(wire::map_error(ec));
                }
                storage.names[count] = text;
                ++count;
            }
        }
        return std::span<const std::string_view>{storage.names.data(), count};
    });
}

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
                                             std::pair{"instCategory", &out.inst_category},
                                             std::pair{"instFamily", &out.inst_family},
                                             std::pair{"uly", &out.uly},
                                             std::pair{"stk", &out.strike},
                                             std::pair{"optType", &out.opt_type}}) {
                const auto value = wire::opt_str(obj, key);
                if (!value.has_value()) {
                    return std::unexpected(value.error());
                }
                *field = *value;
            }
            // null until okx issues one
            const auto code = wire::opt_raw(obj, "instIdCode");
            if (!code.has_value()) {
                return std::unexpected(code.error());
            }
            out.inst_id_code = wire::digits_to_u64(*code);
            const auto expiry = wire::opt_ms(obj, "expTime");
            if (!expiry.has_value()) {
                return std::unexpected(expiry.error());
            }
            out.expiry_ms = *expiry;
            storage.instruments[count] = out;
            ++count;
        }
        return std::span<const WireInstrument>{storage.instruments.data(), count};
    });
}

} // namespace raikiri::venues::okx
