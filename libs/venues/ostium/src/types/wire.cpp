#include <raikiri/venues/ostium/wire.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::ostium {

namespace json = raikiri::wire;

namespace {

template <typename Row, typename Read>
std::expected<std::span<const Row>, WireError>
read_rows(json::ondemand::array& rows, std::span<Row> into, Read&& read) {
    std::size_t count = 0;
    for (auto entry : rows) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        json::ondemand::object row;
        if (const auto ec = entry.get_object().get(row); ec != simdjson::SUCCESS) {
            return std::unexpected(json::map_error(ec));
        }
        const auto one = read(row);
        if (!one.has_value()) {
            return std::unexpected(one.error());
        }
        into[count] = *one;
        ++count;
    }
    return std::span<const Row>{into.data(), count};
}

std::expected<WirePair, WireError> read_pair(json::ondemand::object& row) {
    const auto id = json::req_str(row, "id");
    const auto from = json::req_str(row, "from");
    const auto to = json::req_str(row, "to");
    const auto symbol = json::req_str(row, "symbol");
    if (!id.has_value() || !from.has_value() || !to.has_value() || !symbol.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    WirePair out{.id = *id, .symbol = *symbol, .from = *from, .to = *to, .group = {}};
    json::ondemand::object group;
    if (json::find(row, "group").get_object().get(group) == simdjson::SUCCESS) {
        if (const auto name = json::opt_str(group, "name"); name.has_value()) {
            out.group = *name;
        }
    }
    return out;
}

std::expected<std::string_view, WireError> req_number(json::ondemand::object& obj, std::string_view key) {
    const auto raw = json::opt_raw(obj, key);
    if (!raw.has_value()) {
        return std::unexpected(raw.error());
    }
    if (raw->empty() || *raw == "null") {
        return std::unexpected(WireError::MissingField);
    }
    return *raw;
}

std::expected<WireQuote, WireError> read_quote(json::ondemand::object& row) {
    const auto pair = json::req_str(row, "pair");
    if (!pair.has_value()) {
        return std::unexpected(pair.error());
    }
    const auto bid = req_number(row, "bid");
    const auto mid = req_number(row, "mid");
    const auto ask = req_number(row, "ask");
    const auto open = json::opt_bool(row, "isMarketOpen");
    const auto stamped = json::opt_u64(row, "timestampMs");
    const auto generated = json::opt_u64(row, "generatedTsMs");
    if (!bid.has_value() || !mid.has_value() || !ask.has_value() || !open.has_value() || !stamped.has_value() ||
        !generated.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    return WireQuote{.pair = *pair,
                     .bid = *bid,
                     .mid = *mid,
                     .ask = *ask,
                     .ts_ms = *generated != 0 ? *generated : *stamped,
                     .open = *open};
}

std::expected<WireRollover, WireError> read_rollover(json::ondemand::object& row) {
    const auto id = json::req_str(row, "id");
    const auto long_oi = json::req_str(row, "longOI");
    const auto short_oi = json::req_str(row, "shortOI");
    const auto pure = json::req_str(row, "lastRolloverLongPure");
    const auto premium = json::req_str(row, "brokerPremium");
    const auto negative = json::opt_bool(row, "isNegativeRolloverAllowed");
    if (!id.has_value() || !long_oi.has_value() || !short_oi.has_value() || !pure.has_value() || !premium.has_value() ||
        !negative.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    return WireRollover{.id = *id,
                        .long_oi = *long_oi,
                        .short_oi = *short_oi,
                        .long_pure = *pure,
                        .premium = *premium,
                        .negative_allowed = *negative};
}

std::expected<WireFill, WireError> read_fill(json::ondemand::object& row) {
    WireFill out{};
    bool buy_seen = false;
    const auto read = json::for_each_field(
        row, [&](std::string_view key, json::ondemand::value& value) -> std::expected<void, WireError> {
            if (key == "pair") {
                json::ondemand::object pair;
                if (const auto ec = value.get_object().get(pair); ec != simdjson::SUCCESS) {
                    return std::unexpected(json::map_error(ec));
                }
                const auto id = json::req_str(pair, "id");
                if (!id.has_value()) {
                    return std::unexpected(id.error());
                }
                out.pair_id = *id;
                return {};
            }
            if (key == "isBuy") {
                const auto buy = json::bool_of(value);
                if (!buy.has_value()) {
                    return std::unexpected(buy.error());
                }
                out.is_buy = *buy;
                buy_seen = true;
                return {};
            }
            const auto text = json::str_of(value);
            if (!text.has_value()) {
                return std::unexpected(text.error());
            }
            if (key == "id") {
                out.id = *text;
            } else if (key == "orderAction") {
                out.action = *text;
            } else if (key == "priceAfterImpact") {
                out.price = *text;
            } else if (key == "tradeNotional") {
                out.size = *text;
            } else if (key == "closePercent") {
                out.close_percent = static_cast<std::uint32_t>(json::digits_to_u64(*text));
            } else if (key == "executedAt") {
                out.executed_s = json::digits_to_u64(*text);
            }
            return {};
        });
    if (!read.has_value()) {
        return std::unexpected(read.error());
    }
    if (out.id.empty() || out.executed_s == 0 || out.pair_id.empty() || out.action.empty() || out.price.empty() ||
        out.size.empty() || !buy_seen) {
        return std::unexpected(WireError::MissingField);
    }
    return out;
}

template <typename Row, typename Read>
std::expected<std::span<const Row>, WireError>
graph_rows(std::string_view body, std::string_view key, std::span<Row> into, Read&& read) {
    auto root = json::session().open_object(body);
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    auto data = json::req_object(*root, "data");
    if (!data.has_value()) {
        return std::unexpected(data.error());
    }
    auto rows = json::req_array(*data, key);
    if (!rows.has_value()) {
        return std::unexpected(rows.error());
    }
    return read_rows(*rows, into, read);
}

} // namespace

std::expected<std::span<const WirePair>, WireError> parse_pairs(std::string_view body, PairBuffer& storage) {
    return json::guarded([&]() -> std::expected<std::span<const WirePair>, WireError> {
        auto root = json::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto rows = json::req_array(*root, "pairs");
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        return read_rows(*rows, std::span<WirePair>{storage.pairs}, read_pair);
    });
}

Frame parse_frame(std::string_view frame) {
    const auto kind = json::guarded([&]() -> std::expected<Frame, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto type = json::opt_str(*root, "type");
        if (!type.has_value()) {
            return std::unexpected(type.error());
        }
        if (*type == "tick") {
            return Frame::Tick;
        }
        if (*type == "snapshot") {
            return Frame::Snapshot;
        }
        if (*type == "gap") {
            return Frame::Gap;
        }
        if (*type == "ack") {
            return Frame::Ack;
        }
        if (*type == "error") {
            return Frame::Error;
        }
        return Frame::Unknown;
    });
    return kind.value_or(Frame::Unknown);
}

std::expected<std::span<const WireQuote>, WireError> parse_quotes(std::string_view frame, QuoteBuffer& storage) {
    return json::guarded([&]() -> std::expected<std::span<const WireQuote>, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto type = json::req_str(*root, "type");
        if (!type.has_value()) {
            return std::unexpected(type.error());
        }
        if (*type == "snapshot") {
            auto rows = json::req_array(*root, "data");
            if (!rows.has_value()) {
                return std::unexpected(rows.error());
            }
            return read_rows(*rows, std::span<WireQuote>{storage.quotes}, read_quote);
        }
        if (*type != "tick") {
            return std::unexpected(WireError::MissingField);
        }
        auto row = json::req_object(*root, "data");
        if (!row.has_value()) {
            return std::unexpected(row.error());
        }
        const auto one = read_quote(*row);
        if (!one.has_value()) {
            return std::unexpected(one.error());
        }
        storage.quotes[0] = *one;
        return std::span<const WireQuote>{storage.quotes.data(), 1};
    });
}

std::expected<std::span<const WireRollover>, WireError> parse_rollover(std::string_view body, RolloverBuffer& storage) {
    return json::guarded([&]() -> std::expected<std::span<const WireRollover>, WireError> {
        return graph_rows(body, "pairs", std::span<WireRollover>{storage.rows}, read_rollover);
    });
}

std::expected<std::span<const WireFill>, WireError> parse_fills(std::string_view body, FillBuffer& storage) {
    return json::guarded([&]() -> std::expected<std::span<const WireFill>, WireError> {
        return graph_rows(body, "orders", std::span<WireFill>{storage.fills}, read_fill);
    });
}

} // namespace raikiri::venues::ostium
