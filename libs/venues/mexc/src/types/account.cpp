#include <raikiri/venues/mexc/account.hpp>
#include <raikiri/venues/mexc/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::mexc {

namespace {

using wire::guarded;

std::expected<void, WireError> read_record(wire::ondemand::object& obj, WireFundingRecord& out) {
    out.symbol = field(obj, "symbol");
    out.funding = field(obj, "funding");
    out.rate = field(obj, "rate");
    if (out.symbol.empty() || out.funding.empty() || out.rate.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    out.position_type = int_field(obj, "positionType");
    obj.reset();
    const auto settled = scalar_ms(obj, "settleTime");
    if (!settled.has_value()) {
        return std::unexpected(settled.error());
    }
    out.settle_ms = *settled;
    obj.reset();
    const auto id = wire::opt_u64(obj, "id");
    if (!id.has_value()) {
        return std::unexpected(id.error());
    }
    out.id = *id;
    return {};
}

} // namespace

std::expected<std::span<const WireFundingRecord>, WireError> parse_funding_records(std::string_view body,
                                                                                   FundingRecordBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireFundingRecord>, WireError> {
        auto root = wire::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = wire::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        auto array = wire::req_array(*data, "resultList");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto element : *array) {
            if (count == k_max_funding_records) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object obj;
            if (const auto ec = element.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WireFundingRecord row{};
            if (const auto ok = read_record(obj, row); !ok.has_value()) {
                return std::unexpected(ok.error());
            }
            storage.records[count] = row;
            ++count;
        }
        return std::span<const WireFundingRecord>{storage.records.data(), count};
    });
}

} // namespace raikiri::venues::mexc
