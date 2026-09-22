#include <raikiri/venues/phemex/market_data.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <charconv>
#include <initializer_list>
#include <utility>

namespace raikiri::venues::phemex {

namespace {

using wire::guarded;
using wire::session;

std::expected<std::string_view, WireError> text_of(wire::ondemand::value& value) {
    return wire::str_of(value);
}

std::expected<std::int64_t, WireError> int_of(wire::ondemand::value& value) {
    std::int64_t out = 0;
    if (const auto ec = value.get_int64().get(out); ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    return out;
}

// [ts_ns, side, price, qty], where price and qty are strings on the real
// channel and integers on the scaled one.
template <typename Print, typename Read>
std::expected<Print, WireError> read_print(wire::ondemand::array row, Read read_number) {
    Print out{};
    std::size_t field = 0;
    for (auto item : row) {
        wire::ondemand::value value;
        if (const auto ec = item.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        if (field == 0) {
            if (const auto ec = value.get_uint64().get(out.ts_ns); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
        } else if (field == 1) {
            const auto side = wire::str_of(value);
            if (!side.has_value()) {
                return std::unexpected(side.error());
            }
            if (*side != "Buy" && *side != "Sell") {
                return std::unexpected(WireError::WrongType);
            }
            out.taker_bought = *side == "Buy";
        } else if (field == 2 || field == 3) {
            const auto number = read_number(value);
            if (!number.has_value()) {
                return std::unexpected(number.error());
            }
            if (field == 2) {
                if constexpr (requires { out.price_ep; }) {
                    out.price_ep = *number;
                } else {
                    out.price = *number;
                }
            } else {
                if constexpr (requires { out.qty_ev; }) {
                    out.qty_ev = *number;
                } else {
                    out.qty = *number;
                }
            }
        }
        ++field;
    }
    if (field < 4) {
        return std::unexpected(WireError::ShortArray);
    }
    return out;
}

template <typename Print, typename Read>
std::expected<WireTrades<Print>, WireError> parse_trades(std::string_view frame,
                                                         std::string_view key,
                                                         PrintBuffer<Print>& storage,
                                                         Read read_number) {
    return guarded([&]() -> std::expected<WireTrades<Print>, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireTrades<Print> out{};
        const auto symbol = wire::req_str(*root, "symbol");
        const auto type = wire::req_str(*root, "type");
        if (!symbol.has_value() || !type.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.symbol = *symbol;
        out.snapshot = *type == "snapshot";
        auto rows = wire::req_array(*root, key);
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        std::size_t count = 0;
        for (auto entry : *rows) {
            if (count == k_max_prints) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::array row;
            if (const auto ec = entry.get_array().get(row); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            const auto print = read_print<Print>(row, read_number);
            if (!print.has_value()) {
                return std::unexpected(print.error());
            }
            storage.prints[count] = *print;
            ++count;
        }
        out.prints = {storage.prints.data(), count};
        return out;
    });
}

std::expected<void, WireError> read_columns(wire::ondemand::array fields, PackColumns& columns) {
    PackColumns out{};
    int index = 0;
    for (auto item : fields) {
        wire::ondemand::value value;
        if (const auto ec = item.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        const auto name = wire::str_of(value);
        if (!name.has_value()) {
            return std::unexpected(name.error());
        }
        if (*name == "symbol") {
            out.symbol = index;
        } else if (*name == "markRp") {
            out.mark = index;
        } else if (*name == "fundingRateRr") {
            out.rate = index;
        } else if (*name == "predFundingRateRr") {
            out.predicted = index;
        } else if (*name == "volumeRq") {
            out.volume = index;
        } else if (*name == "turnoverRv") {
            out.turnover = index;
        }
        ++index;
    }
    if (!out.known()) {
        return std::unexpected(WireError::MissingField);
    }
    columns = out;
    return {};
}

std::expected<WirePackRow, WireError> read_pack_row(wire::ondemand::array row,
                                                    const PackColumns& columns) {
    WirePackRow out{};
    const std::array<std::pair<int, std::string_view*>, 6> wanted{{
        {columns.symbol, &out.symbol},
        {columns.mark, &out.mark},
        {columns.rate, &out.rate},
        {columns.predicted, &out.predicted},
        {columns.volume, &out.volume},
        {columns.turnover, &out.turnover},
    }};
    int index = 0;
    for (auto item : row) {
        wire::ondemand::value value;
        if (const auto ec = item.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        for (const auto& [column, into] : wanted) {
            if (column != index) {
                continue;
            }
            const auto text = wire::str_of(value);
            if (!text.has_value()) {
                return std::unexpected(text.error());
            }
            *into = *text;
            break;
        }
        ++index;
    }
    if (index <= columns.symbol || out.symbol.empty()) {
        return std::unexpected(WireError::ShortArray);
    }
    return out;
}

// Absent is nullopt; present but not an integer is an error.
std::expected<std::optional<std::int64_t>, WireError> opt_int(wire::ondemand::object& obj,
                                                              std::string_view key) {
    const auto raw = wire::opt_raw(obj, key);
    if (!raw.has_value()) {
        return std::unexpected(raw.error());
    }
    if (raw->empty() || *raw == "null") {
        return std::optional<std::int64_t>{};
    }
    std::int64_t out = 0;
    const auto [stop, ec] = std::from_chars(raw->data(), raw->data() + raw->size(), out);
    if (ec != std::errc{} || stop != raw->data() + raw->size()) {
        return std::unexpected(WireError::WrongType);
    }
    return std::optional<std::int64_t>{out};
}

struct IntField {
    std::string_view key;
    std::optional<std::int64_t>* into;
};

std::expected<void, WireError> read_ints(wire::ondemand::object& obj,
                                         std::initializer_list<IntField> wanted) {
    for (const auto& want : wanted) {
        const auto value = opt_int(obj, want.key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *want.into = *value;
    }
    return {};
}

} // namespace

std::expected<WireTrades<WireRealPrint>, WireError>
parse_real_trades(std::string_view frame, PrintBuffer<WireRealPrint>& storage) {
    return parse_trades(frame, "trades_p", storage, text_of);
}

std::expected<WireTrades<WireScaledPrint>, WireError>
parse_scaled_trades(std::string_view frame, PrintBuffer<WireScaledPrint>& storage) {
    return parse_trades(frame, "trades", storage, int_of);
}

std::expected<WirePack, WireError>
parse_perp_pack(std::string_view frame, PackColumns& columns, PackBuffer& storage) {
    return guarded([&]() -> std::expected<WirePack, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        if (wire::has_field(*root, "fields")) {
            auto fields = wire::req_array(*root, "fields");
            if (!fields.has_value()) {
                return std::unexpected(fields.error());
            }
            if (const auto ok = read_columns(*fields, columns); !ok.has_value()) {
                return std::unexpected(ok.error());
            }
        }
        if (!columns.known()) {
            return std::unexpected(WireError::MissingField);
        }
        WirePack out{};
        const auto ts = wire::opt_u64(*root, "timestamp");
        if (!ts.has_value()) {
            return std::unexpected(ts.error());
        }
        out.ts_ns = *ts;
        auto rows = wire::req_array(*root, "data");
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        std::size_t count = 0;
        for (auto entry : *rows) {
            if (count == k_max_pack_rows) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::array row;
            if (const auto ec = entry.get_array().get(row); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            const auto parsed = read_pack_row(row, columns);
            if (!parsed.has_value()) {
                return std::unexpected(parsed.error());
            }
            storage.rows[count] = *parsed;
            ++count;
        }
        out.rows = {storage.rows.data(), count};
        return out;
    });
}

std::expected<WireInverseTicker, WireError> parse_inverse_ticker(std::string_view frame) {
    return guarded([&]() -> std::expected<WireInverseTicker, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireInverseTicker out{};
        const auto ts = wire::opt_u64(*root, "timestamp");
        if (!ts.has_value()) {
            return std::unexpected(ts.error());
        }
        out.ts_ns = *ts;
        auto body = wire::req_object(*root, "market24h");
        if (!body.has_value()) {
            return std::unexpected(body.error());
        }
        const auto symbol = wire::req_str(*body, "symbol");
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        out.symbol = *symbol;
        if (const auto ok = read_ints(*body,
                                      {{"fundingRate", &out.rate_er},
                                       {"predFundingRate", &out.predicted_er},
                                       {"markPrice", &out.mark_ep},
                                       {"volume", &out.volume},
                                       {"turnover", &out.turnover_ev}});
            !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        return out;
    });
}

std::expected<WireSpotTicker, WireError> parse_spot_ticker(std::string_view frame) {
    return guarded([&]() -> std::expected<WireSpotTicker, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireSpotTicker out{};
        const auto ts = wire::opt_u64(*root, "timestamp");
        if (!ts.has_value()) {
            return std::unexpected(ts.error());
        }
        out.ts_ns = *ts;
        auto body = wire::req_object(*root, "spot_market24h");
        if (!body.has_value()) {
            return std::unexpected(body.error());
        }
        const auto symbol = wire::req_str(*body, "symbol");
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        out.symbol = *symbol;
        if (const auto ok = read_ints(
                *body, {{"volumeEv", &out.volume_ev}, {"turnoverEv", &out.turnover_ev}});
            !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        return out;
    });
}

} // namespace raikiri::venues::phemex
