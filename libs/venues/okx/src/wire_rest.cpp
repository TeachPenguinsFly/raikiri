#include "wire_detail.hpp"

namespace raikiri::venues::okx {

namespace {

using detail::guarded;
using detail::session;

} // namespace

std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body) {
    return guarded([&]() -> std::expected<RestEnvelope, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto code = detail::req_str(*root, "code");
        const auto msg = detail::opt_str(*root, "msg");
        if (!code.has_value() || !msg.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        // Zero is success, so an unreadable code must not be allowed to
        // decay into one: a body whose envelope we cannot read is an error.
        const auto value = detail::digits_to_u64(*code);
        if (value == 0 && *code != "0") {
            return std::unexpected(WireError::WrongType);
        }
        return RestEnvelope{.code = static_cast<std::int64_t>(value), .msg = *msg};
    });
}

std::expected<std::span<const WireInstrument>, WireError>
parse_instruments(std::string_view body, InstrumentBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireInstrument>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = detail::req_array(*root, "data");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_instruments) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WireInstrument out{};
            const auto inst_id = detail::req_str(obj, "instId");
            const auto inst_type = detail::req_str(obj, "instType");
            if (!inst_id.has_value() || !inst_type.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.inst_id = *inst_id;
            out.inst_type = *inst_type;
            // A row that is not live yet ships every numeric field as "",
            // so these are optional at the wire layer and the mapper is what
            // refuses the row.
            for (const auto& [key, field] : {std::pair{"state", &out.state},
                                             std::pair{"tickSz", &out.tick_size},
                                             std::pair{"lotSz", &out.lot_size},
                                             std::pair{"minSz", &out.min_size},
                                             std::pair{"ctVal", &out.ct_val},
                                             std::pair{"ctMult", &out.ct_mult}}) {
                const auto value = detail::opt_str(obj, key);
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

std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body) {
    return guarded([&]() -> std::expected<std::uint64_t, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return detail::with_first_data(
            *root, [](detail::ondemand::object& obj) -> std::expected<std::uint64_t, WireError> {
                const auto ts = detail::opt_ms(obj, "ts");
                if (!ts.has_value()) {
                    return std::unexpected(ts.error());
                }
                if (*ts == 0) {
                    return std::unexpected(WireError::MissingField);
                }
                return *ts;
            });
    });
}

} // namespace raikiri::venues::okx
