#include <raikiri/venues/bitunix/wire.hpp>
#include <raikiri/wire/json.hpp>

#include <charconv>

namespace raikiri::venues::bitunix {

namespace {

template <typename Row, typename Read>
std::expected<std::span<const Row>, WireError>
read_rows(wire::ondemand::array& rows, std::span<Row> into, Read&& read) {
    std::size_t count = 0;
    for (auto entry : rows) {
        wire::ondemand::object row;
        if (const auto ec = entry.get_object().get(row); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        const auto one = read(row);
        if (!one.has_value()) {
            return std::unexpected(one.error());
        }
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        into[count] = *one;
        ++count;
    }
    return std::span<const Row>{into.data(), count};
}

template <typename Row, typename Read>
std::expected<std::span<const Row>, WireError>
parse_data_rows(std::string_view body, std::span<Row> into, Read&& read) {
    return wire::guarded([&]() -> std::expected<std::span<const Row>, WireError> {
        auto root = wire::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto rows = wire::req_array(*root, "data");
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        return read_rows(*rows, into, read);
    });
}

std::expected<std::uint64_t, WireError> req_count(wire::ondemand::object& row, std::string_view key) {
    wire::ondemand::value value;
    if (const auto ec = wire::find(row, key).get(value); ec != simdjson::SUCCESS) {
        return std::unexpected(WireError::MissingField);
    }
    return wire::u64_of(value);
}

std::expected<std::size_t, WireError>
read_side(wire::ondemand::object& data, std::string_view key, std::span<WireLevel> into) {
    wire::ondemand::array side;
    if (wire::find(data, key).get_array().get(side) != simdjson::SUCCESS) {
        return std::size_t{0};
    }
    std::size_t count = 0;
    for (auto entry : side) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        wire::ondemand::array pair;
        if (const auto ec = entry.get_array().get(pair); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        std::array<std::string_view, 2> cells{};
        std::size_t n = 0;
        for (auto cell : pair) {
            if (n == cells.size()) {
                break;
            }
            wire::ondemand::value value;
            if (const auto ec = cell.get(value); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            const auto text = wire::str_of(value);
            if (!text.has_value()) {
                return std::unexpected(text.error());
            }
            cells[n] = *text;
            ++n;
        }
        if (n != cells.size()) {
            return std::unexpected(WireError::ShortArray);
        }
        into[count] = WireLevel{.price = cells[0], .qty = cells[1]};
        ++count;
    }
    return count;
}

std::expected<std::size_t, WireError>
read_objects(wire::ondemand::object& data, std::string_view key, std::span<WireLevel> into) {
    auto side = wire::req_array(data, key);
    if (!side.has_value()) {
        return std::unexpected(side.error());
    }
    const auto rows = read_rows(*side, into, [](wire::ondemand::object& row) -> std::expected<WireLevel, WireError> {
        const auto price = wire::req_str(row, "price");
        const auto qty = wire::req_str(row, "volume");
        if (!price.has_value() || !qty.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        return WireLevel{.price = *price, .qty = *qty};
    });
    if (!rows.has_value()) {
        return std::unexpected(rows.error());
    }
    return rows->size();
}

struct Envelope {
    std::string_view symbol;
    std::uint64_t ts_ms{0};
};

std::expected<Envelope, WireError> read_envelope(wire::ondemand::object& root) {
    const auto symbol = wire::req_str(root, "symbol");
    const auto ts = wire::opt_u64(root, "ts");
    if (!symbol.has_value() || !ts.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    return Envelope{.symbol = *symbol, .ts_ms = *ts};
}

} // namespace

std::expected<WireStatus, WireError> parse_status(std::string_view body) {
    return wire::guarded([&]() -> std::expected<WireStatus, WireError> {
        auto root = wire::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        wire::ondemand::value code;
        if (wire::find(*root, "code").get(code) != simdjson::SUCCESS) {
            return std::unexpected(WireError::MissingField);
        }
        WireStatus out{};
        if (const auto number = wire::i64_of(code); number.has_value()) {
            out.code = *number;
        } else {
            const auto text = wire::str_of(code);
            if (!text.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            const auto* const last = text->data() + text->size();
            const auto [end, ec] = std::from_chars(text->data(), last, out.code);
            if (ec != std::errc{} || end != last) {
                return std::unexpected(WireError::WrongType);
            }
        }
        if (const auto msg = wire::opt_str(*root, "msg"); msg.has_value()) {
            out.msg = *msg;
        }
        return out;
    });
}

std::expected<std::span<const WirePair>, WireError> parse_pairs(std::string_view body, PairBuffer& storage) {
    return parse_data_rows(
        body, std::span<WirePair>{storage.rows}, [](wire::ondemand::object& row) -> std::expected<WirePair, WireError> {
            WirePair out{};
            const auto symbol = wire::req_str(row, "symbol");
            const auto base = wire::req_str(row, "base");
            const auto quote = wire::req_str(row, "quote");
            const auto min = wire::req_str(row, "minTradeVolume");
            const auto base_precision = req_count(row, "basePrecision");
            const auto quote_precision = req_count(row, "quotePrecision");
            const auto status = wire::req_str(row, "symbolStatus");
            const auto api = wire::opt_bool(row, "isApiSupported");
            if (!symbol.has_value() || !base.has_value() || !quote.has_value() || !min.has_value() ||
                !base_precision.has_value() || !quote_precision.has_value() || !status.has_value() ||
                !api.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.symbol = *symbol;
            out.base = *base;
            out.quote = *quote;
            out.min_qty = *min;
            out.base_precision = *base_precision;
            out.quote_precision = *quote_precision;
            out.status = *status;
            out.api = *api;
            out.max_funding = wire::opt_str(row, "maxFundingRate").value_or(std::string_view{});
            out.min_funding = wire::opt_str(row, "minFundingRate").value_or(std::string_view{});
            return out;
        });
}

std::expected<std::span<const WirePair>, WireError> parse_spot_pairs(std::string_view body, PairBuffer& storage) {
    return parse_data_rows(
        body, std::span<WirePair>{storage.rows}, [](wire::ondemand::object& row) -> std::expected<WirePair, WireError> {
            WirePair out{};
            const auto symbol = wire::req_str(row, "symbol");
            const auto base = wire::req_str(row, "base");
            const auto quote = wire::req_str(row, "quote");
            const auto base_precision = req_count(row, "basePrecision");
            const auto quote_precision = req_count(row, "quotePrecision");
            const auto min = wire::req_str(row, "minVolume");
            const auto open = req_count(row, "isOpen");
            if (!symbol.has_value() || !base.has_value() || !quote.has_value() || !base_precision.has_value() ||
                !quote_precision.has_value() || !min.has_value() || !open.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.symbol = *symbol;
            out.base = *base;
            out.quote = *quote;
            out.min_qty = *min;
            out.base_precision = *base_precision;
            out.quote_precision = *quote_precision;
            out.status = *open == 1 ? std::string_view{"OPEN"} : std::string_view{"CLOSED"};
            out.api = *open == 1;
            return out;
        });
}

std::expected<std::span<const WireFunding>, WireError> parse_funding(std::string_view body, FundingBuffer& storage) {
    return parse_data_rows(body,
                           std::span<WireFunding>{storage.rows},
                           [](wire::ondemand::object& row) -> std::expected<WireFunding, WireError> {
                               WireFunding out{};
                               const auto symbol = wire::req_str(row, "symbol");
                               const auto mark = wire::opt_str(row, "markPrice");
                               const auto index = wire::opt_str(row, "indexPrice");
                               const auto rate = wire::req_str(row, "fundingRate");
                               const auto interval = req_count(row, "fundingInterval");
                               const auto next = wire::opt_ms(row, "nextFundingTime");
                               if (!symbol.has_value() || !mark.has_value() || !index.has_value() ||
                                   !rate.has_value() || !interval.has_value() || !next.has_value()) {
                                   return std::unexpected(WireError::MissingField);
                               }
                               out.symbol = *symbol;
                               out.mark = *mark;
                               out.index = *index;
                               out.rate = *rate;
                               out.interval_h = *interval;
                               out.next_ms = *next;
                               return out;
                           });
}

std::expected<std::span<const WireTicker>, WireError> parse_tickers(std::string_view body, TickerBuffer& storage) {
    return parse_data_rows(body,
                           std::span<WireTicker>{storage.rows},
                           [](wire::ondemand::object& row) -> std::expected<WireTicker, WireError> {
                               const auto symbol = wire::req_str(row, "symbol");
                               const auto quote = wire::opt_str(row, "quoteVol");
                               const auto base = wire::opt_str(row, "baseVol");
                               if (!symbol.has_value() || !quote.has_value() || !base.has_value()) {
                                   return std::unexpected(WireError::MissingField);
                               }
                               return WireTicker{.symbol = *symbol, .base_volume = *base, .quote_volume = *quote};
                           });
}

WireHead parse_head(std::string_view frame) {
    const auto head = wire::guarded([&]() -> std::expected<WireHead, WireError> {
        auto root = wire::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireHead out{};
        const auto ch = wire::opt_str(*root, "ch");
        if (ch.has_value() && !ch->empty()) {
            out.ch = *ch;
            if (ch->starts_with("depth_book")) {
                out.channel = Channel::Book;
            } else if (*ch == "trade") {
                out.channel = Channel::Trade;
            }
            return out;
        }
        const auto op = wire::opt_str(*root, "op");
        if (!op.has_value()) {
            return out;
        }
        if (*op == "ping") {
            out.channel = Channel::Pong;
        } else if (*op == "connect") {
            out.channel = Channel::Connect;
        } else if (*op == "subscribe") {
            out.channel = Channel::Refused;
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
        const auto envelope = read_envelope(*root);
        if (!envelope.has_value()) {
            return std::unexpected(envelope.error());
        }
        auto data = wire::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        const auto bids = read_side(*data, "b", std::span<WireLevel>{storage.bids});
        if (!bids.has_value()) {
            return std::unexpected(bids.error());
        }
        const auto asks = read_side(*data, "a", std::span<WireLevel>{storage.asks});
        if (!asks.has_value()) {
            return std::unexpected(asks.error());
        }
        return WireBook{.symbol = envelope->symbol,
                        .ts_ms = envelope->ts_ms,
                        .bids = std::span<const WireLevel>{storage.bids.data(), *bids},
                        .asks = std::span<const WireLevel>{storage.asks.data(), *asks}};
    });
}

std::expected<WireBook, WireError> parse_spot_depth(std::string_view body, LevelBuffer& storage) {
    return wire::guarded([&]() -> std::expected<WireBook, WireError> {
        auto root = wire::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = wire::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        const auto asks = read_objects(*data, "asks", std::span<WireLevel>{storage.asks});
        if (!asks.has_value()) {
            return std::unexpected(asks.error());
        }
        const auto bids = read_objects(*data, "bids", std::span<WireLevel>{storage.bids});
        if (!bids.has_value()) {
            return std::unexpected(bids.error());
        }
        return WireBook{.symbol = {},
                        .ts_ms = 0,
                        .bids = std::span<const WireLevel>{storage.bids.data(), *bids},
                        .asks = std::span<const WireLevel>{storage.asks.data(), *asks}};
    });
}

bool is_backfill(const WireTrades& trades, std::uint64_t subscribed_ms) noexcept {
    // the pool paces its subscribe frames, so the window covers a few of them
    constexpr std::uint64_t k_backfill_window_ms = 10'000;
    return trades.ts_ms < subscribed_ms + k_backfill_window_ms;
}

std::expected<WireTrades, WireError> parse_trades(std::string_view frame, PrintBuffer& storage) {
    return wire::guarded([&]() -> std::expected<WireTrades, WireError> {
        auto root = wire::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto envelope = read_envelope(*root);
        if (!envelope.has_value()) {
            return std::unexpected(envelope.error());
        }
        auto rows = wire::req_array(*root, "data");
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        const auto read = [](wire::ondemand::object& row) -> std::expected<WirePrint, WireError> {
            const auto price = wire::req_str(row, "p");
            const auto qty = wire::req_str(row, "v");
            const auto side = wire::req_str(row, "s");
            if (!price.has_value() || !qty.has_value() || !side.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            return WirePrint{.price = *price, .qty = *qty, .side = *side};
        };
        const auto prints = read_rows(*rows, std::span<WirePrint>{storage.prints}, read);
        if (!prints.has_value()) {
            return std::unexpected(prints.error());
        }
        return WireTrades{.symbol = envelope->symbol, .ts_ms = envelope->ts_ms, .prints = *prints};
    });
}

} // namespace raikiri::venues::bitunix
