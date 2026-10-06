#include <raikiri/venues/cryptocom/catalogue.hpp>
#include <raikiri/wire/json.hpp>

#include "fields.hpp"

namespace raikiri::venues::cryptocom {

namespace {

FieldResult read_row(ondemand::object& row, WireInstrument& out) {
    return each_field(row, [&](std::string_view key, ondemand::value& value) -> FieldResult {
        if (key == "symbol") {
            return text_into(value, out.symbol);
        }
        if (key == "inst_type") {
            return text_into(value, out.inst_type);
        }
        if (key == "base_ccy") {
            return text_into(value, out.base_ccy);
        }
        if (key == "quote_ccy") {
            return text_into(value, out.quote_ccy);
        }
        if (key == "price_tick_size") {
            return text_into(value, out.price_tick_size);
        }
        if (key == "qty_tick_size") {
            return text_into(value, out.qty_tick_size);
        }
        if (key == "contract_size") {
            return text_into(value, out.contract_size);
        }
        if (key == "underlying_symbol") {
            return text_into(value, out.underlying);
        }
        if (key == "product_type") {
            return text_into(value, out.product_type);
        }
        if (key == "tradable") {
            return bool_into(value, out.tradable);
        }
        return {};
    });
}

FieldResult read_rows(ondemand::value& data, InstrumentBuffer& storage, std::size_t& count) {
    ondemand::array rows;
    if (const auto ec = data.get_array().get(rows); ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    for (auto entry : rows) {
        if (count == k_max_instruments) {
            return std::unexpected(WireError::TooManyEntries);
        }
        ondemand::object row;
        if (const auto ec = entry.get_object().get(row); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        WireInstrument out{};
        if (auto read = read_row(row, out); !read.has_value()) {
            return read;
        }
        if (out.symbol.empty()) {
            return std::unexpected(WireError::MissingField);
        }
        storage.rows[count] = out;
        ++count;
    }
    return {};
}

} // namespace

std::expected<std::span<const WireInstrument>, WireError> parse_instruments(std::string_view body,
                                                                            InstrumentBuffer& storage) {
    return wire::guarded([&]() -> std::expected<std::span<const WireInstrument>, WireError> {
        auto root = wire::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        std::size_t count = 0;
        bool saw_data = false;
        const auto read = each_field(*root, [&](std::string_view key, ondemand::value& value) -> FieldResult {
            if (key != "result") {
                return {};
            }
            ondemand::object result;
            if (const auto ec = value.get_object().get(result); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            return each_field(result, [&](std::string_view inner, ondemand::value& data) -> FieldResult {
                if (inner != "data") {
                    return {};
                }
                saw_data = true;
                return read_rows(data, storage, count);
            });
        });
        if (!read.has_value()) {
            return std::unexpected(read.error());
        }
        if (!saw_data) {
            return std::unexpected(WireError::MissingField);
        }
        return std::span<const WireInstrument>{storage.rows.data(), count};
    });
}

} // namespace raikiri::venues::cryptocom
