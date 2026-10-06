#include <raikiri/venues/bullish/wire.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::bullish {

namespace json = raikiri::wire;

namespace {

// prices and sizes alternate in one flat array
std::expected<std::size_t, WireError> read_flat(json::ondemand::array& side, std::span<WireLevel> into) {
    std::size_t count = 0;
    std::string_view price;
    bool have_price = false;
    for (auto entry : side) {
        json::ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(json::map_error(ec));
        }
        const auto text = json::str_of(value);
        if (!text.has_value() || text->empty()) {
            return std::unexpected(WireError::WrongType);
        }
        if (!have_price) {
            price = *text;
            have_price = true;
            continue;
        }
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        into[count] = WireLevel{.price = price, .qty = *text};
        ++count;
        have_price = false;
    }
    if (have_price) {
        return std::unexpected(WireError::ShortArray);
    }
    return count;
}

std::expected<json::ondemand::object, WireError> data_of(json::ondemand::object& root) {
    return json::req_object(root, "data");
}

std::expected<std::uint64_t, WireError> stamp_of(json::ondemand::object& obj, std::string_view key) {
    const auto text = json::opt_str(obj, key);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    return json::digits_to_u64(*text);
}

} // namespace

std::expected<std::span<const WireIndex>, WireError> parse_index_prices(std::string_view body, IndexBuffer& storage) {
    return json::guarded([&]() -> std::expected<std::span<const WireIndex>, WireError> {
        auto rows = json::session().open_array(body);
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        std::size_t count = 0;
        for (auto entry : *rows) {
            if (count == storage.rows.size()) {
                return std::unexpected(WireError::TooManyEntries);
            }
            json::ondemand::object row;
            if (const auto ec = entry.get_object().get(row); ec != simdjson::SUCCESS) {
                return std::unexpected(json::map_error(ec));
            }
            const auto price = json::req_str(row, "price");
            const auto asset = json::req_str(row, "assetSymbol");
            if (!price.has_value() || !asset.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            storage.rows[count++] = WireIndex{.asset = *asset, .price = *price};
        }
        return std::span<const WireIndex>{storage.rows.data(), count};
    });
}

std::expected<std::span<const WireMarket>, WireError> parse_markets(std::string_view body, MarketBuffer& storage) {
    return json::guarded([&]() -> std::expected<std::span<const WireMarket>, WireError> {
        auto rows = json::session().open_array(body);
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        std::size_t count = 0;
        for (auto entry : *rows) {
            if (count == storage.markets.size()) {
                return std::unexpected(WireError::TooManyEntries);
            }
            json::ondemand::object row;
            if (const auto ec = entry.get_object().get(row); ec != simdjson::SUCCESS) {
                return std::unexpected(json::map_error(ec));
            }
            const auto symbol = json::req_str(row, "symbol");
            const auto base = json::req_str(row, "baseSymbol");
            const auto quote = json::req_str(row, "quoteSymbol");
            const auto type = json::req_str(row, "marketType");
            const auto tick = json::req_str(row, "tickSize");
            const auto min = json::opt_str(row, "minQuantityLimit");
            const auto multiplier = json::opt_str(row, "contractMultiplier");
            const auto price = json::req_str(row, "pricePrecision");
            const auto quantity = json::req_str(row, "quantityPrecision");
            const auto enabled = json::opt_bool(row, "marketEnabled");
            if (!symbol.has_value() || !base.has_value() || !quote.has_value() || !type.has_value() ||
                !tick.has_value() || !min.has_value() || !multiplier.has_value() || !price.has_value() ||
                !quantity.has_value() || !enabled.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            storage.markets[count] = WireMarket{
                .symbol = *symbol,
                .base = *base,
                .quote = *quote,
                .type = *type,
                .tick = *tick,
                .min_qty = *min,
                .multiplier = *multiplier,
                .price_precision = static_cast<std::uint32_t>(json::digits_to_u64(*price)),
                .quantity_precision = static_cast<std::uint32_t>(json::digits_to_u64(*quantity)),
                .enabled = *enabled};
            ++count;
        }
        return std::span<const WireMarket>{storage.markets.data(), count};
    });
}

std::expected<WireHead, WireError> parse_head(std::string_view frame) {
    return json::guarded([&]() -> std::expected<WireHead, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        if (json::has_field(*root, "error")) {
            return WireHead{.kind = Kind::Refusal};
        }
        if (json::has_field(*root, "result")) {
            return WireHead{.kind = Kind::Reply};
        }
        const auto type = json::opt_str(*root, "type");
        const auto data = json::opt_str(*root, "dataType");
        if (!type.has_value() || !data.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        WireHead out{};
        out.kind = *type == "snapshot" ? Kind::Snapshot : *type == "update" ? Kind::Update : Kind::Unknown;
        if (*data == "V1TALevel2") {
            out.data = DataType::Book;
        } else if (*data == "V1TATickerResponse") {
            out.data = DataType::Tick;
        } else if (*data == "V1TAAnonymousTradeUpdate") {
            out.data = DataType::Trades;
        }
        return out;
    });
}

std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage) {
    return json::guarded([&]() -> std::expected<WireBook, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = data_of(*root);
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireBook out{};
        const auto symbol = json::req_str(*data, "symbol");
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        out.symbol = *symbol;
        for (const bool bid : {true, false}) {
            auto side = json::req_array(*data, bid ? "bids" : "asks");
            if (!side.has_value()) {
                return std::unexpected(side.error());
            }
            auto& into = bid ? storage.bids : storage.asks;
            const auto count = read_flat(*side, into);
            if (!count.has_value()) {
                return std::unexpected(count.error());
            }
            (bid ? out.bids : out.asks) = std::span<const WireLevel>{into.data(), *count};
        }
        auto range = json::req_array(*data, "sequenceNumberRange");
        if (!range.has_value()) {
            return std::unexpected(range.error());
        }
        std::array<std::uint64_t, 2> seq{};
        std::size_t n = 0;
        for (auto entry : *range) {
            if (n == seq.size()) {
                return std::unexpected(WireError::TooManyEntries);
            }
            json::ondemand::value value;
            if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
                return std::unexpected(json::map_error(ec));
            }
            const auto one = json::u64_of(value);
            if (!one.has_value()) {
                return std::unexpected(one.error());
            }
            seq[n] = *one;
            ++n;
        }
        if (n != seq.size() || seq[0] == 0 || seq[1] < seq[0]) {
            return std::unexpected(WireError::ShortArray);
        }
        out.seq_first = seq[0];
        out.seq_last = seq[1];
        const auto ts = stamp_of(*data, "timestamp");
        if (!ts.has_value()) {
            return std::unexpected(ts.error());
        }
        out.ts_ms = *ts;
        return out;
    });
}

std::expected<WireTick, WireError> parse_tick(std::string_view frame) {
    return json::guarded([&]() -> std::expected<WireTick, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = data_of(*root);
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        const auto symbol = json::req_str(*data, "symbol");
        const auto ts = stamp_of(*data, "createdAtTimestamp");
        const auto volume = json::opt_str(*data, "quoteVolume");
        const auto mark = json::opt_str(*data, "markPrice");
        const auto funding = json::opt_str(*data, "fundingRate");
        const auto oi = json::opt_str(*data, "openInterest");
        if (!symbol.has_value() || !ts.has_value() || !volume.has_value() || !mark.has_value() ||
            !funding.has_value() || !oi.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        return WireTick{.symbol = *symbol,
                        .ts_ms = *ts,
                        .quote_volume = *volume,
                        .mark = *mark,
                        .funding_rate = *funding,
                        .open_interest = *oi};
    });
}

std::expected<std::span<const WirePrint>, WireError> parse_trades(std::string_view frame, PrintBuffer& storage) {
    return json::guarded([&]() -> std::expected<std::span<const WirePrint>, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = data_of(*root);
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        auto rows = json::req_array(*data, "trades");
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        std::size_t count = 0;
        for (auto entry : *rows) {
            if (count == storage.prints.size()) {
                return std::unexpected(WireError::TooManyEntries);
            }
            json::ondemand::object row;
            if (const auto ec = entry.get_object().get(row); ec != simdjson::SUCCESS) {
                return std::unexpected(json::map_error(ec));
            }
            const auto symbol = json::req_str(row, "symbol");
            const auto id = json::req_str(row, "tradeId");
            const auto price = json::req_str(row, "price");
            const auto qty = json::req_str(row, "quantity");
            const auto side = json::req_str(row, "side");
            const auto ts = stamp_of(row, "createdAtTimestamp");
            if (!symbol.has_value() || !id.has_value() || !price.has_value() || !qty.has_value() || !side.has_value() ||
                !ts.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            storage.prints[count] = WirePrint{.symbol = *symbol,
                                              .trade_id = json::digits_to_u64(*id),
                                              .price = *price,
                                              .qty = *qty,
                                              .side = *side,
                                              .ts_ms = *ts};
            ++count;
        }
        return std::span<const WirePrint>{storage.prints.data(), count};
    });
}

} // namespace raikiri::venues::bullish
