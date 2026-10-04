#include <raikiri/venues/deribit/account.hpp>
#include <raikiri/venues/deribit/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::deribit {

namespace {

using wire::guarded;
using wire::session;

std::expected<WireLogEntry, WireError> read_log_entry(wire::ondemand::object& obj) {
    WireLogEntry out{};
    const auto id = req_num(obj, "id");
    const auto ts = opt_ms(obj, "timestamp");
    if (!id.has_value() || !ts.has_value()) {
        return std::unexpected(!id.has_value() ? id.error() : ts.error());
    }
    out.id = token_to_u64(*id);
    out.ts_ms = *ts;
    for (const auto& [key, field] : {std::pair{"type", &out.type},
                                     std::pair{"instrument_name", &out.instrument},
                                     std::pair{"currency", &out.currency}}) {
        const auto value = wire::opt_str(obj, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *field = *value;
    }
    const auto funding = opt_num(obj, "total_interest_pl");
    if (!funding.has_value()) {
        return std::unexpected(funding.error());
    }
    out.total_interest_pl = *funding;
    return out;
}

} // namespace

std::expected<WireLogPage, WireError> parse_transaction_log(std::string_view body, LogBuffer& storage) {
    return guarded([&]() -> std::expected<WireLogPage, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto result = wire::req_object(*root, "result");
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        const auto count = read_rows<WireLogEntry>(
            *result, "logs", storage.entries, [](wire::ondemand::object& obj) { return read_log_entry(obj); });
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        const auto continuation = opt_num(*result, "continuation");
        if (!continuation.has_value()) {
            return std::unexpected(continuation.error());
        }
        return WireLogPage{.entries = {storage.entries.data(), *count}, .continuation = token_to_u64(*continuation)};
    });
}

} // namespace raikiri::venues::deribit
