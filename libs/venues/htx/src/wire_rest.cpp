#include "wire_detail.hpp"

namespace raikiri::venues::htx {

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
        const auto status = detail::opt_str(*root, "status");
        if (!status.has_value()) {
            // A Spring 404 body writes `status` as the number 404. Refusing it
            // here is what makes the caller fall back to the HTTP status
            // instead of reading a routing failure as a venue verdict.
            return std::unexpected(status.error());
        }
        if (*status == "ok") {
            return RestEnvelope{.ok = true, .code = {}, .msg = {}};
        }
        if (*status != "error") {
            return std::unexpected(WireError::WrongType);
        }
        // The api family answers err_code/err_msg with a numeric code; the
        // ex/market family answers err-code/err-msg with a hyphenated word.
        // One body never carries both.
        auto code = detail::opt_raw(*root, "err_code");
        auto msg = detail::opt_str(*root, "err_msg");
        if (!code.has_value() || !msg.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        if (code->empty()) {
            code = detail::opt_raw(*root, "err-code");
            msg = detail::opt_str(*root, "err-msg");
            if (!code.has_value() || !msg.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
        }
        if (code->empty()) {
            // A failure that names nothing is not something to act on, and an
            // envelope reporting one with an empty code is exactly the
            // partly-filled struct this layer exists to avoid.
            return std::unexpected(WireError::MissingField);
        }
        return RestEnvelope{.ok = false, .code = detail::unquote(*code), .msg = *msg};
    });
}

std::expected<std::span<const WireContract>, WireError> parse_contracts(std::string_view body,
                                                                        ContractBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireContract>, WireError> {
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
            WireContract out{};
            const auto symbol = detail::req_str(obj, "symbol");
            const auto code = detail::req_str(obj, "contract_code");
            const auto size = detail::opt_raw(obj, "contract_size");
            const auto tick = detail::opt_raw(obj, "price_tick");
            const auto delivery = detail::opt_str(obj, "delivery_time");
            const auto status = detail::opt_i64(obj, "contract_status");
            const auto settlement = detail::opt_ms_str(obj, "settlement_date");
            const auto period = detail::opt_str(obj, "settlement_period");
            const auto business = detail::opt_str(obj, "business_type");
            const auto type = detail::opt_str(obj, "contract_type");
            if (!symbol.has_value() || !code.has_value() || !size.has_value() ||
                !tick.has_value() || !delivery.has_value() || !status.has_value() ||
                !settlement.has_value() || !period.has_value() || !business.has_value() ||
                !type.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.symbol = *symbol;
            out.contract_code = *code;
            out.contract_size = *size;
            out.price_tick = *tick;
            out.delivery_time = *delivery;
            out.contract_status = *status;
            out.settlement_ms = *settlement;
            out.settlement_period_h = static_cast<std::int64_t>(detail::digits_to_u64(*period));
            out.business_type = *business;
            out.contract_type = *type;
            storage.contracts[count] = out;
            ++count;
        }
        return std::span<const WireContract>{storage.contracts.data(), count};
    });
}

std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body) {
    return guarded([&]() -> std::expected<std::uint64_t, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto status = detail::opt_str(*root, "status");
        if (!status.has_value() || *status != "ok") {
            return std::unexpected(WireError::MissingField);
        }
        const auto ts = detail::opt_u64(*root, "ts");
        if (!ts.has_value()) {
            return std::unexpected(ts.error());
        }
        if (*ts == 0) {
            return std::unexpected(WireError::MissingField);
        }
        return *ts;
    });
}

} // namespace raikiri::venues::htx
