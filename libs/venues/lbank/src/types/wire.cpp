#include <raikiri/venues/lbank/wire.hpp>
#include <raikiri/wire/json.hpp>

#include <cstdint>

namespace raikiri::venues::lbank {

namespace {

// a field that is sometimes a string and sometimes a number, as its text
std::expected<std::string_view, WireError> text_of(wire::ondemand::object& obj, std::string_view key) {
    const auto raw = wire::opt_raw(obj, key);
    if (!raw.has_value()) {
        return std::unexpected(raw.error());
    }
    auto text = *raw;
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        text = text.substr(1, text.size() - 2);
    }
    return text == "null" ? std::string_view{} : text;
}

std::expected<std::string_view, WireError> req_text(wire::ondemand::object& obj, std::string_view key) {
    const auto text = text_of(obj, key);
    if (text.has_value() && text->empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return text;
}

template <typename Row, typename Read>
std::expected<std::span<const Row>, WireError>
read_rows(wire::ondemand::array& rows, std::span<Row> into, Read&& read) {
    std::size_t count = 0;
    for (auto entry : rows) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        wire::ondemand::object row;
        if (const auto ec = entry.get_object().get(row); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
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

template <typename Row, typename Read>
std::expected<std::span<const Row>, WireError>
parse_data_rows(std::string_view body, std::span<Row> into, std::string_view key, Read&& read) {
    return wire::guarded([&]() -> std::expected<std::span<const Row>, WireError> {
        auto root = wire::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto rows = wire::req_array(*root, key);
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        return read_rows(*rows, into, read);
    });
}

// rows of a QuantDo push: [{"table":..., "data":{...}}, ...]
template <typename Row, typename Read>
std::expected<std::span<const Row>, WireError>
read_results(wire::ondemand::object& root, std::span<Row> into, Read&& read) {
    auto rows = wire::req_array(root, "result");
    if (!rows.has_value()) {
        return std::unexpected(rows.error());
    }
    return read_rows(*rows, into, [&](wire::ondemand::object& row) {
        auto data = wire::req_object(row, "data");
        if (!data.has_value()) {
            return decltype(read(*data)){std::unexpected(data.error())};
        }
        return read(*data);
    });
}

std::uint64_t u64_text(std::string_view text) noexcept {
    return wire::digits_to_u64(text);
}

} // namespace

std::expected<std::span<const WirePerpInstrument>, WireError> parse_perp_instruments(std::string_view body,
                                                                                     PerpInstrumentBuffer& storage) {
    return parse_data_rows(body,
                           std::span<WirePerpInstrument>{storage.rows},
                           "data",
                           [](wire::ondemand::object& row) -> std::expected<WirePerpInstrument, WireError> {
                               const auto symbol = req_text(row, "symbol");
                               const auto base = req_text(row, "baseCurrency");
                               const auto quote = req_text(row, "priceCurrency");
                               const auto tick = req_text(row, "priceTick");
                               const auto lot = req_text(row, "volumeTick");
                               const auto multiple = text_of(row, "volumeMultiple");
                               const auto min_cost = text_of(row, "minOrderCost");
                               const auto suspend = text_of(row, "needSuspend");
                               if (!symbol || !base || !quote || !tick || !lot || !multiple || !min_cost || !suspend) {
                                   return std::unexpected(WireError::MissingField);
                               }
                               return WirePerpInstrument{.symbol = *symbol,
                                                         .base = *base,
                                                         .quote = *quote,
                                                         .price_tick = *tick,
                                                         .volume_tick = *lot,
                                                         .multiple = *multiple,
                                                         .min_cost = *min_cost,
                                                         .suspended = !suspend->empty() && *suspend != "0"};
                           });
}

std::expected<std::span<const WirePerpMarket>, WireError> parse_perp_markets(std::string_view body,
                                                                             PerpMarketBuffer& storage) {
    return parse_data_rows(body,
                           std::span<WirePerpMarket>{storage.rows},
                           "data",
                           [](wire::ondemand::object& row) -> std::expected<WirePerpMarket, WireError> {
                               const auto symbol = req_text(row, "symbol");
                               const auto interval = text_of(row, "positionFeeTime");
                               const auto next = text_of(row, "nextFeeTime");
                               if (!symbol || !interval || !next) {
                                   return std::unexpected(WireError::MissingField);
                               }
                               return WirePerpMarket{.symbol = *symbol,
                                                     .interval_s = static_cast<std::uint32_t>(u64_text(*interval)),
                                                     .next_fee_ms = u64_text(*next)};
                           });
}

WireHead parse_head(std::string_view frame) {
    const auto head = wire::guarded([&]() -> std::expected<WireHead, WireError> {
        auto root = wire::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto action = wire::opt_str(*root, "action");
        const auto code = wire::opt_i64(*root, "errorCode");
        if (!action.has_value() || !code.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        WireHead out{.action = Action::Unknown, .error_code = *code};
        if (*action == "PushMarketOrder") {
            out.action = Action::Book;
        } else if (*action == "PushMarketTrade") {
            out.action = Action::Trade;
        } else if (*action == "PushMarketDataOverView") {
            out.action = Action::Overview;
        } else if (*action == "RecvTopicAction") {
            out.action = Action::Ack;
        } else if (action->empty()) {
            out.action = Action::Note;
        }
        return out;
    });
    return head.value_or(WireHead{});
}

std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage) {
    return wire::guarded([&]() -> std::expected<WireBook, WireError> {
        auto root = wire::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireBook out{};
        const auto bno = wire::opt_u64(*root, "bNo");
        if (!bno.has_value()) {
            return std::unexpected(bno.error());
        }
        out.bno = *bno;
        out.snapshot = !wire::has_field(*root, "bNo");
        const auto index = wire::opt_str(*root, out.snapshot ? "errorMsg" : "index");
        if (!index.has_value()) {
            return std::unexpected(index.error());
        }
        out.index = *index;
        const auto levels = read_results(*root,
                                         std::span<WireLevel>{storage.levels},
                                         [](wire::ondemand::object& data) -> std::expected<WireLevel, WireError> {
                                             const auto instrument = wire::req_str(data, "InstrumentID");
                                             const auto direction = wire::req_str(data, "Direction");
                                             const auto price = wire::req_str(data, "Price");
                                             const auto volume = wire::req_str(data, "Volume");
                                             const auto tick = wire::opt_str(data, "Tick");
                                             if (!instrument || !direction || !price || !volume || !tick) {
                                                 return std::unexpected(WireError::MissingField);
                                             }
                                             return WireLevel{.instrument = *instrument,
                                                              .price = *price,
                                                              .volume = *volume,
                                                              .tick = *tick,
                                                              .ask = *direction == "1"};
                                         });
        if (!levels.has_value()) {
            return std::unexpected(levels.error());
        }
        out.levels = *levels;
        return out;
    });
}

std::expected<WireRestDepth, WireError> parse_market_order(std::string_view body, RestDepthBuffer& storage) {
    return wire::guarded([&]() -> std::expected<WireRestDepth, WireError> {
        auto root = wire::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = wire::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        const auto read = [](wire::ondemand::object& row) -> std::expected<WireDepthRow, WireError> {
            const auto volume = wire::req_str(row, "volume");
            const auto price = wire::req_str(row, "price");
            if (!volume || !price) {
                return std::unexpected(WireError::MissingField);
            }
            return WireDepthRow{.price = *price, .volume = *volume};
        };
        auto asks = wire::req_array(*data, "asks");
        if (!asks.has_value()) {
            return std::unexpected(asks.error());
        }
        const auto ask_rows = read_rows(*asks, std::span<WireDepthRow>{storage.asks}, read);
        if (!ask_rows.has_value()) {
            return std::unexpected(ask_rows.error());
        }
        auto bids = wire::req_array(*data, "bids");
        if (!bids.has_value()) {
            return std::unexpected(bids.error());
        }
        const auto bid_rows = read_rows(*bids, std::span<WireDepthRow>{storage.bids}, read);
        if (!bid_rows.has_value()) {
            return std::unexpected(bid_rows.error());
        }
        return WireRestDepth{.bids = *bid_rows, .asks = *ask_rows};
    });
}

std::expected<std::span<const WireTrade>, WireError> parse_trades(std::string_view frame, TradeBuffer& storage) {
    return wire::guarded([&]() -> std::expected<std::span<const WireTrade>, WireError> {
        auto root = wire::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return read_results(*root,
                            std::span<WireTrade>{storage.trades},
                            [](wire::ondemand::object& data) -> std::expected<WireTrade, WireError> {
                                const auto id = wire::req_str(data, "TradeID");
                                const auto instrument = wire::req_str(data, "InstrumentID");
                                const auto direction = wire::req_str(data, "Direction");
                                const auto price = wire::req_str(data, "Price");
                                const auto volume = wire::req_str(data, "Volume");
                                const auto time = wire::req_str(data, "TradeTime");
                                if (!id || !instrument || !direction || !price || !volume || !time) {
                                    return std::unexpected(WireError::MissingField);
                                }
                                return WireTrade{.instrument = *instrument,
                                                 .trade_id = *id,
                                                 .price = *price,
                                                 .volume = *volume,
                                                 .time_s = u64_text(*time),
                                                 .buy = *direction == "0"};
                            });
    });
}

std::expected<std::span<const WireOverview>, WireError> parse_overview(std::string_view frame,
                                                                       OverviewBuffer& storage) {
    return wire::guarded([&]() -> std::expected<std::span<const WireOverview>, WireError> {
        auto root = wire::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return read_results(*root,
                            std::span<WireOverview>{storage.rows},
                            [](wire::ondemand::object& data) -> std::expected<WireOverview, WireError> {
                                WireOverview out{};
                                const auto read = wire::for_each_field(
                                    data, [&](std::string_view key, wire::ondemand::value value) {
                                        std::string_view* into = nullptr;
                                        if (key == "InstrumentID") {
                                            into = &out.instrument;
                                        } else if (key == "MarkedPrice") {
                                            into = &out.mark;
                                        } else if (key == "UnderlyingPrice") {
                                            into = &out.index;
                                        } else if (key == "PositionFeeRate") {
                                            into = &out.fee_rate;
                                        } else if (key == "Volume24") {
                                            into = &out.volume_24h;
                                        } else if (key == "Turnover24") {
                                            into = &out.turnover_24h;
                                        } else if (key == "UpdateTime") {
                                            if (const auto text = wire::str_of(value); text.has_value()) {
                                                out.update_s = u64_text(*text);
                                            }
                                            return;
                                        } else {
                                            return;
                                        }
                                        if (const auto text = wire::str_of(value); text.has_value()) {
                                            *into = *text;
                                        }
                                    });
                                if (!read.has_value()) {
                                    return std::unexpected(read.error());
                                }
                                if (out.instrument.empty()) {
                                    return std::unexpected(WireError::MissingField);
                                }
                                return out;
                            });
    });
}

std::expected<std::span<const WireSpotPair>, WireError> parse_spot_pairs(std::string_view body,
                                                                         SpotPairBuffer& storage) {
    return parse_data_rows(body,
                           std::span<WireSpotPair>{storage.rows},
                           "data",
                           [](wire::ondemand::object& row) -> std::expected<WireSpotPair, WireError> {
                               const auto symbol = req_text(row, "symbol");
                               const auto price = req_text(row, "priceAccuracy");
                               const auto qty = req_text(row, "quantityAccuracy");
                               const auto min_qty = text_of(row, "minTranQua");
                               const auto min_amount = text_of(row, "minOrderAmount");
                               if (!symbol || !price || !qty || !min_qty || !min_amount) {
                                   return std::unexpected(WireError::MissingField);
                               }
                               return WireSpotPair{.symbol = *symbol,
                                                   .price_places = *price,
                                                   .qty_places = *qty,
                                                   .min_qty = *min_qty,
                                                   .min_amount = *min_amount};
                           });
}

std::expected<std::span<const std::string_view>, WireError> parse_spot_ticker_symbols(std::string_view body,
                                                                                      SymbolBuffer& storage) {
    return parse_data_rows(body,
                           std::span<std::string_view>{storage.symbols},
                           "data",
                           [](wire::ondemand::object& row) -> std::expected<std::string_view, WireError> {
                               return req_text(row, "symbol");
                           });
}

std::expected<std::span<const WireSpotTick>, WireError> parse_spot_tickers(std::string_view body,
                                                                           SpotTickBuffer& storage) {
    return parse_data_rows(body,
                           std::span<WireSpotTick>{storage.rows},
                           "data",
                           [](wire::ondemand::object& row) -> std::expected<WireSpotTick, WireError> {
                               WireSpotTick out{};
                               const auto pair = req_text(row, "symbol");
                               if (!pair.has_value()) {
                                   return std::unexpected(pair.error());
                               }
                               out.pair = *pair;
                               auto tick = wire::req_object(row, "ticker");
                               if (!tick.has_value()) {
                                   return std::unexpected(tick.error());
                               }
                               const auto volume = req_text(*tick, "vol");
                               const auto turnover = req_text(*tick, "turnover");
                               if (!volume || !turnover) {
                                   return std::unexpected(WireError::MissingField);
                               }
                               out.volume = *volume;
                               out.turnover = *turnover;
                               const auto ts = wire::opt_u64(row, "timestamp");
                               if (!ts.has_value()) {
                                   return std::unexpected(ts.error());
                               }
                               out.ts_ms = *ts;
                               return out;
                           });
}

SpotChannel parse_spot_channel(std::string_view frame) {
    const auto kind = wire::guarded([&]() -> std::expected<SpotChannel, WireError> {
        auto root = wire::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto type = wire::opt_str(*root, "type");
        if (!type.has_value()) {
            return std::unexpected(type.error());
        }
        if (*type == "depth") {
            return SpotChannel::Depth;
        }
        if (*type == "trade") {
            return SpotChannel::Trade;
        }
        if (*type == "tick") {
            return SpotChannel::Tick;
        }
        if (!type->empty()) {
            return SpotChannel::Unknown;
        }
        const auto action = wire::opt_str(*root, "action");
        if (action.has_value() && *action == "pong") {
            return SpotChannel::Pong;
        }
        const auto status = wire::opt_str(*root, "status");
        if (status.has_value() && *status == "error") {
            return SpotChannel::Error;
        }
        return SpotChannel::Unknown;
    });
    return kind.value_or(SpotChannel::Unknown);
}

namespace {

std::expected<std::size_t, WireError> read_pairs(wire::ondemand::array& side, std::span<WirePair> into) {
    std::size_t count = 0;
    for (auto entry : side) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        wire::ondemand::array level;
        if (const auto ec = entry.get_array().get(level); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        std::array<std::string_view, 2> cells{};
        std::size_t n = 0;
        for (auto cell : level) {
            wire::ondemand::value value;
            if (cell.get(value) != simdjson::SUCCESS || n == cells.size()) {
                return std::unexpected(WireError::WrongType);
            }
            const auto text = wire::str_of(value);
            if (!text.has_value()) {
                return std::unexpected(text.error());
            }
            cells[n] = *text;
            ++n;
        }
        if (n != cells.size()) {
            return std::unexpected(WireError::MissingField);
        }
        into[count] = WirePair{.price = cells[0], .qty = cells[1]};
        ++count;
    }
    return count;
}

} // namespace

std::expected<WireSpotDepth, WireError> parse_spot_depth(std::string_view frame, SpotLevelBuffer& storage) {
    return wire::guarded([&]() -> std::expected<WireSpotDepth, WireError> {
        auto root = wire::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireSpotDepth out{};
        auto depth = wire::req_object(*root, "depth");
        if (!depth.has_value()) {
            return std::unexpected(depth.error());
        }
        auto asks = wire::req_array(*depth, "asks");
        if (!asks.has_value()) {
            return std::unexpected(asks.error());
        }
        const auto ask_count = read_pairs(*asks, std::span<WirePair>{storage.asks});
        if (!ask_count.has_value()) {
            return std::unexpected(ask_count.error());
        }
        auto bids = wire::req_array(*depth, "bids");
        if (!bids.has_value()) {
            return std::unexpected(bids.error());
        }
        const auto bid_count = read_pairs(*bids, std::span<WirePair>{storage.bids});
        if (!bid_count.has_value()) {
            return std::unexpected(bid_count.error());
        }
        out.asks = std::span<const WirePair>{storage.asks.data(), *ask_count};
        out.bids = std::span<const WirePair>{storage.bids.data(), *bid_count};
        const auto pair = wire::req_str(*root, "pair");
        const auto ts = wire::opt_str(*root, "TS");
        const auto ds = wire::opt_str(*root, "ds");
        if (!pair || !ts || !ds) {
            return std::unexpected(WireError::MissingField);
        }
        out.pair = *pair;
        out.ts_ms = ds->empty() ? beijing_to_ms(*ts) : u64_text(*ds);
        return out;
    });
}

std::expected<WireSpotTrade, WireError> parse_spot_trade(std::string_view frame) {
    return wire::guarded([&]() -> std::expected<WireSpotTrade, WireError> {
        auto root = wire::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireSpotTrade out{};
        auto trade = wire::req_object(*root, "trade");
        if (!trade.has_value()) {
            return std::unexpected(trade.error());
        }
        const auto volume = req_text(*trade, "volume");
        const auto price = req_text(*trade, "price");
        const auto direction = wire::req_str(*trade, "direction");
        const auto ts = wire::req_str(*trade, "TS");
        if (!volume || !price || !direction || !ts) {
            return std::unexpected(WireError::MissingField);
        }
        out.volume = *volume;
        out.price = *price;
        out.buy = *direction == "buy";
        out.ts_ms = beijing_to_ms(*ts);
        const auto pair = wire::req_str(*root, "pair");
        if (!pair.has_value()) {
            return std::unexpected(pair.error());
        }
        out.pair = *pair;
        return out;
    });
}

std::expected<WireSpotTick, WireError> parse_spot_tick(std::string_view frame) {
    return wire::guarded([&]() -> std::expected<WireSpotTick, WireError> {
        auto root = wire::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireSpotTick out{};
        auto tick = wire::req_object(*root, "tick");
        if (!tick.has_value()) {
            return std::unexpected(tick.error());
        }
        const auto volume = req_text(*tick, "vol");
        const auto turnover = req_text(*tick, "turnover");
        if (!volume || !turnover) {
            return std::unexpected(WireError::MissingField);
        }
        out.volume = *volume;
        out.turnover = *turnover;
        const auto pair = wire::req_str(*root, "pair");
        const auto ts = wire::opt_str(*root, "TS");
        if (!pair || !ts) {
            return std::unexpected(WireError::MissingField);
        }
        out.pair = *pair;
        out.ts_ms = beijing_to_ms(*ts);
        return out;
    });
}

namespace {

constexpr bool digits(std::string_view text) noexcept {
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return !text.empty();
}

constexpr std::int64_t days_from_civil(std::int64_t y, std::int64_t m, std::int64_t d) noexcept {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;
    const std::int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

} // namespace

std::uint64_t beijing_to_ms(std::string_view text) noexcept {
    if (text.size() < 19 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':') {
        return 0;
    }
    const auto field = [&](std::size_t at, std::size_t len) -> std::int64_t {
        const auto part = text.substr(at, len);
        return digits(part) ? static_cast<std::int64_t>(wire::digits_to_u64(part)) : -1;
    };
    const auto year = field(0, 4);
    const auto month = field(5, 2);
    const auto day = field(8, 2);
    const auto hour = field(11, 2);
    const auto minute = field(14, 2);
    const auto second = field(17, 2);
    if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 ||
        minute > 59 || second < 0 || second > 60) {
        return 0;
    }
    std::int64_t millis = 0;
    if (text.size() > 19) {
        if (text[19] != '.') {
            return 0;
        }
        const auto frac = text.substr(20, 3);
        if (!digits(frac)) {
            return 0;
        }
        millis = static_cast<std::int64_t>(wire::digits_to_u64(frac));
        for (auto n = frac.size(); n < 3; ++n) {
            millis *= 10;
        }
    }
    const auto seconds = days_from_civil(year, month, day) * 86'400 + hour * 3'600 + minute * 60 + second - 8 * 3'600;
    return seconds < 0 ? 0 : static_cast<std::uint64_t>(seconds * 1'000 + millis);
}

} // namespace raikiri::venues::lbank
