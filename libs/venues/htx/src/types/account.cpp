#include <raikiri/venues/htx/account.hpp>
#include <raikiri/venues/htx/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::htx {

namespace {

using wire::guarded;
using wire::session;

} // namespace

std::expected<std::uint64_t, WireError> parse_spot_account_id(std::string_view body) {
    return guarded([&]() -> std::expected<std::uint64_t, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = wire::req_array(*root, "data");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        for (auto entry : *array) {
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            const auto type = wire::opt_str(obj, "type");
            const auto state = wire::opt_str(obj, "state");
            const auto id = wire::opt_u64(obj, "id");
            if (!type.has_value() || !state.has_value() || !id.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            // An account carries margin, otc and point rows beside the spot
            // one; placing an order against the wrong id is answered
            // "invalid-parameter" rather than routed.
            if (*type == "spot" && *state == "working" && *id != 0) {
                return *id;
            }
        }
        return std::unexpected(WireError::MissingField);
    });
}

std::expected<std::span<const WireFinancialRecord>, WireError>
parse_financial_records(std::string_view body, FinancialRecordBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireFinancialRecord>, WireError> {
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
            if (count == k_max_financial_records) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            const auto code = wire::req_str(obj, "contract_code");
            const auto asset = wire::opt_str(obj, "asset");
            const auto amount = num_of(obj, "amount");
            const auto type = wire::opt_i64(obj, "type");
            const auto query_id = wire::opt_u64(obj, "query_id");
            const auto ts = wire::opt_u64(obj, "ts");
            if (!code.has_value() || !asset.has_value() || !amount.has_value() ||
                !type.has_value() || !query_id.has_value() || !ts.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            storage.records[count] = {.contract_code = *code,
                                      .asset = *asset,
                                      .amount = *amount,
                                      .query_id = *query_id,
                                      .ts_ms = *ts,
                                      .type = *type};
            ++count;
        }
        return std::span<const WireFinancialRecord>{storage.records.data(), count};
    });
}

} // namespace raikiri::venues::htx
