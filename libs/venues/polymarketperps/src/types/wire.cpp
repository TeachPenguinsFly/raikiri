#include <raikiri/venues/polymarketperps/wire.hpp>
#include <raikiri/wire/json.hpp>

#include <charconv>

namespace raikiri::venues::polymarketperps {

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

std::uint64_t iid_of(std::string_view channel) noexcept {
    const auto cut = channel.find("::");
    return cut == std::string_view::npos ? 0 : json::digits_to_u64(channel.substr(cut + 2));
}

std::expected<WireInstrument, WireError> read_instrument(json::ondemand::object& row) {
    const auto iid = json::opt_u64(row, "instrument_id");
    const auto type = json::req_str(row, "instrument_type");
    const auto category = json::req_str(row, "category");
    const auto symbol = json::req_str(row, "symbol");
    const auto close_only = json::opt_bool(row, "close_only");
    const auto base = json::req_str(row, "base_asset");
    const auto qty = json::opt_u64(row, "quantity_decimals");
    const auto price = json::opt_u64(row, "price_decimals");
    const auto min = json::opt_str(row, "min_notional");
    if (!iid.has_value() || !type.has_value() || !category.has_value() || !symbol.has_value() ||
        !close_only.has_value() || !base.has_value() || !qty.has_value() || !price.has_value() || !min.has_value() ||
        *iid == 0) {
        return std::unexpected(WireError::MissingField);
    }
    return WireInstrument{.iid = *iid,
                          .symbol = *symbol,
                          .base = *base,
                          .category = *category,
                          .type = *type,
                          .min_notional = *min,
                          .price_decimals = static_cast<std::uint32_t>(*price),
                          .quantity_decimals = static_cast<std::uint32_t>(*qty),
                          .close_only = *close_only};
}

std::expected<std::size_t, WireError> read_side(json::ondemand::array& side, std::span<WireLevel> into) {
    std::size_t count = 0;
    for (auto entry : side) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        json::ondemand::array pair;
        if (const auto ec = entry.get_array().get(pair); ec != simdjson::SUCCESS) {
            return std::unexpected(json::map_error(ec));
        }
        std::array<std::string_view, 2> cells{};
        std::size_t n = 0;
        for (auto cell : pair) {
            json::ondemand::value value;
            if (cell.get(value) != simdjson::SUCCESS || n == cells.size()) {
                return std::unexpected(WireError::MissingField);
            }
            const auto text = json::str_of(value);
            if (!text.has_value()) {
                return std::unexpected(text.error());
            }
            cells[n] = *text;
            ++n;
        }
        if (n != cells.size()) {
            return std::unexpected(WireError::MissingField);
        }
        into[count] = WireLevel{.price = cells[0], .qty = cells[1]};
        ++count;
    }
    return count;
}

// the envelope every channel frame shares
struct Envelope {
    std::uint64_t iid{0};
    std::uint64_t sq{0};
    std::uint64_t ets{0};
};

std::expected<Envelope, WireError> envelope(json::ondemand::object& root) {
    const auto channel = json::req_str(root, "ch");
    const auto ets = json::opt_u64(root, "ets");
    const auto sq = json::opt_u64(root, "sq");
    if (!channel.has_value() || !ets.has_value() || !sq.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    return Envelope{.iid = iid_of(*channel), .sq = *sq, .ets = *ets};
}

std::expected<WireBbo, WireError> read_bbo(json::ondemand::object& data,
                                           std::string_view id_key,
                                           std::string_view bid_key,
                                           std::string_view bid_qty_key,
                                           std::string_view ask_key,
                                           std::string_view ask_qty_key) {
    const auto iid = json::opt_u64(data, id_key);
    const auto bid = json::opt_str(data, bid_key);
    const auto bid_qty = json::opt_str(data, bid_qty_key);
    const auto ask = json::opt_str(data, ask_key);
    const auto ask_qty = json::opt_str(data, ask_qty_key);
    if (!iid.has_value() || !bid.has_value() || !bid_qty.has_value() || !ask.has_value() || !ask_qty.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    return WireBbo{
        .iid = *iid, .sq = 0, .ts_ms = 0, .bid = *bid, .bid_qty = *bid_qty, .ask = *ask, .ask_qty = *ask_qty};
}

} // namespace

std::expected<std::span<const WireInstrument>, WireError> parse_instruments(std::string_view body,
                                                                            InstrumentBuffer& storage) {
    return json::guarded([&]() -> std::expected<std::span<const WireInstrument>, WireError> {
        auto rows = json::session().open_array(body);
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        return read_rows(*rows, std::span<WireInstrument>{storage.rows}, read_instrument);
    });
}

Frame parse_frame(std::string_view frame) {
    const auto kind = json::guarded([&]() -> std::expected<Frame, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto channel = json::opt_str(*root, "ch");
        if (!channel.has_value()) {
            return std::unexpected(channel.error());
        }
        if (channel->empty()) {
            return json::has_field(*root, "id") ? Frame::Answer : Frame::Unknown;
        }
        const auto name = channel->substr(0, channel->find("::"));
        if (name == "bbo") {
            return Frame::Bbo;
        }
        if (name == "book") {
            return Frame::Book;
        }
        if (name == "trades") {
            return Frame::Trades;
        }
        if (name == "tickers") {
            return Frame::Ticker;
        }
        if (name == "statistics") {
            return Frame::Statistics;
        }
        return Frame::Unknown;
    });
    return kind.value_or(Frame::Unknown);
}

std::size_t count_refusals(std::string_view frame) {
    const auto refused = json::guarded([&]() -> std::expected<std::size_t, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        json::ondemand::array answers;
        if (json::find(*root, "data").get_array().get(answers) != simdjson::SUCCESS) {
            return std::size_t{0};
        }
        std::size_t count = 0;
        for (auto entry : answers) {
            json::ondemand::object answer;
            if (entry.get_object().get(answer) != simdjson::SUCCESS) {
                continue;
            }
            const auto status = json::opt_str(answer, "status");
            count += status.has_value() && *status != "ok" ? 1U : 0U;
        }
        return count;
    });
    return refused.value_or(0);
}

std::expected<WireBbo, WireError> parse_bbo(std::string_view frame) {
    return json::guarded([&]() -> std::expected<WireBbo, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto head = envelope(*root);
        if (!head.has_value()) {
            return std::unexpected(head.error());
        }
        auto data = json::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        auto out = read_bbo(*data, "iid", "bp", "bq", "ap", "aq");
        if (!out.has_value()) {
            return std::unexpected(out.error());
        }
        out->sq = head->sq;
        out->ts_ms = head->ets;
        return out;
    });
}

std::expected<std::span<const WireBbo>, WireError> parse_bbo_rows(std::string_view body, BboBuffer& storage) {
    return json::guarded([&]() -> std::expected<std::span<const WireBbo>, WireError> {
        auto rows = json::session().open_array(body);
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        return read_rows(*rows, std::span<WireBbo>{storage.rows}, [](json::ondemand::object& row) {
            auto out = read_bbo(row, "instrument_id", "bid_price", "bid_quantity", "ask_price", "ask_quantity");
            if (out.has_value()) {
                out->ts_ms = json::opt_u64(row, "timestamp").value_or(0);
            }
            return out;
        });
    });
}

std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage) {
    return json::guarded([&]() -> std::expected<WireBook, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto head = envelope(*root);
        if (!head.has_value()) {
            return std::unexpected(head.error());
        }
        auto data = json::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        auto asks = json::req_array(*data, "a");
        if (!asks.has_value()) {
            return std::unexpected(asks.error());
        }
        const auto na = read_side(*asks, storage.asks);
        if (!na.has_value()) {
            return std::unexpected(na.error());
        }
        auto bids = json::req_array(*data, "b");
        if (!bids.has_value()) {
            return std::unexpected(bids.error());
        }
        const auto nb = read_side(*bids, storage.bids);
        if (!nb.has_value()) {
            return std::unexpected(nb.error());
        }
        if (head->iid == 0) {
            return std::unexpected(WireError::MissingField);
        }
        return WireBook{.iid = head->iid,
                        .sq = head->sq,
                        .ts_ms = head->ets,
                        .bids = std::span<const WireLevel>{storage.bids.data(), *nb},
                        .asks = std::span<const WireLevel>{storage.asks.data(), *na}};
    });
}

std::expected<std::span<const WirePrint>, WireError> parse_trades(std::string_view frame, PrintBuffer& storage) {
    return json::guarded([&]() -> std::expected<std::span<const WirePrint>, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto rows = json::req_array(*root, "data");
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        return read_rows(
            *rows,
            std::span<WirePrint>{storage.prints},
            [](json::ondemand::object& row) -> std::expected<WirePrint, WireError> {
                const auto iid = json::opt_u64(row, "iid");
                const auto side = json::req_str(row, "side");
                const auto price = json::req_str(row, "p");
                const auto qty = json::req_str(row, "qty");
                const auto settlement = json::opt_bool(row, "settlement");
                const auto ts = json::opt_u64(row, "ts");
                if (!iid.has_value() || !side.has_value() || !price.has_value() || !qty.has_value() ||
                    !settlement.has_value() || !ts.has_value()) {
                    return std::unexpected(WireError::MissingField);
                }
                return WirePrint{
                    .iid = *iid, .side = *side, .price = *price, .qty = *qty, .ts_ms = *ts, .settlement = *settlement};
            });
    });
}

std::expected<WireTicker, WireError> parse_ticker(std::string_view frame) {
    return json::guarded([&]() -> std::expected<WireTicker, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto head = envelope(*root);
        if (!head.has_value()) {
            return std::unexpected(head.error());
        }
        auto data = json::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        const auto iid = json::opt_u64(*data, "iid");
        const auto index = json::opt_str(*data, "idx");
        const auto mark = json::opt_str(*data, "mark");
        const auto oi = json::opt_str(*data, "oi");
        const auto funding = json::opt_str(*data, "fr");
        const auto next = json::opt_u64(*data, "nxf");
        if (!iid.has_value() || !index.has_value() || !mark.has_value() || !oi.has_value() || !funding.has_value() ||
            !next.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        return WireTicker{.iid = *iid,
                          .ts_ms = head->ets,
                          .index = *index,
                          .mark = *mark,
                          .open_interest = *oi,
                          .funding = *funding,
                          .next_funding_ms = *next};
    });
}

std::expected<WireStatistics, WireError> parse_statistics(std::string_view frame) {
    return json::guarded([&]() -> std::expected<WireStatistics, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto head = envelope(*root);
        if (!head.has_value()) {
            return std::unexpected(head.error());
        }
        auto data = json::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        const auto iid = json::opt_u64(*data, "iid");
        const auto volume = json::opt_str(*data, "vol");
        if (!iid.has_value() || !volume.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        return WireStatistics{.iid = *iid, .ts_ms = head->ets, .volume = *volume};
    });
}

} // namespace raikiri::venues::polymarketperps
