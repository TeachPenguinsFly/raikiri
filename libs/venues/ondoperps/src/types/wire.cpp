#include <raikiri/venues/ondoperps/wire.hpp>
#include <raikiri/wire/json.hpp>

#include <charconv>

namespace raikiri::venues::ondoperps {

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

// the rows of an update frame, read under the frame's `timestamp`
template <typename Row, typename Read>
std::expected<std::span<const Row>, WireError> update_rows(std::string_view frame, std::span<Row> into, Read&& read) {
    return json::guarded([&]() -> std::expected<std::span<const Row>, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto stamp = json::opt_str(*root, "timestamp");
        if (!stamp.has_value()) {
            return std::unexpected(stamp.error());
        }
        const auto at = iso_to_ns(*stamp).value_or(0);
        auto rows = json::req_array(*root, "data");
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        return read_rows(*rows, into, [&](json::ondemand::object& row) { return read(row, at); });
    });
}

template <typename Row, typename Read>
std::expected<std::span<const Row>, WireError> result_rows(std::string_view body, std::span<Row> into, Read&& read) {
    return json::guarded([&]() -> std::expected<std::span<const Row>, WireError> {
        auto root = json::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto rows = json::req_array(*root, "result");
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        return read_rows(*rows, into, read);
    });
}

std::uint64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3U : 9U)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::uint64_t>(era * 146097 + static_cast<std::int64_t>(doe) - 719468);
}

bool digits(std::string_view text, std::size_t at, std::size_t n, unsigned& out) noexcept {
    if (at + n > text.size()) {
        return false;
    }
    const auto* first = text.data() + at;
    const auto [end, ec] = std::from_chars(first, first + n, out);
    return ec == std::errc{} && end == first + n;
}

std::expected<WireMarket, WireError> read_market(json::ondemand::object& row) {
    WireMarket out{};
    const auto read = json::for_each_field(
        row, [&](std::string_view key, json::ondemand::value& value) -> std::expected<void, WireError> {
            if (key == "pair") {
                json::ondemand::object pair;
                if (value.get_object().get(pair) != simdjson::SUCCESS) {
                    return std::unexpected(WireError::MissingField);
                }
                const auto base = json::req_str(pair, "base");
                const auto quote = json::req_str(pair, "quote");
                if (!base.has_value() || !quote.has_value()) {
                    return std::unexpected(WireError::MissingField);
                }
                out.base = *base;
                out.quote = *quote;
            } else if (key == "tags") {
                json::ondemand::array tags;
                if (value.get_array().get(tags) == simdjson::SUCCESS) {
                    for (auto tag : tags) {
                        json::ondemand::value one;
                        if (tag.get(one) == simdjson::SUCCESS && out.tag.empty()) {
                            out.tag = json::str_of(one).value_or(std::string_view{});
                        }
                    }
                }
            } else if (key == "disabled") {
                out.disabled = json::bool_of(value).value_or(false);
            } else if (key == "market" || key == "quoteIncrement" || key == "baseIncrement" || key == "makerFee" ||
                       key == "takerFee" || key == "fundingRateCap") {
                const auto text = json::str_of(value);
                if (!text.has_value()) {
                    return std::unexpected(text.error());
                }
                if (key == "market") {
                    out.market = *text;
                } else if (key == "quoteIncrement") {
                    out.tick = *text;
                } else if (key == "baseIncrement") {
                    out.lot = *text;
                } else if (key == "makerFee") {
                    out.maker_fee = *text;
                } else if (key == "takerFee") {
                    out.taker_fee = *text;
                } else {
                    out.funding_cap = *text;
                }
            }
            return {};
        });
    if (!read.has_value()) {
        return std::unexpected(read.error());
    }
    if (out.market.empty() || out.base.empty() || out.tick.empty() || out.lot.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return out;
}

std::expected<std::span<const WireLevel>, WireError>
read_side(json::ondemand::object& row, std::string_view key, std::span<WireLevel> pool, std::size_t& used) {
    auto side = json::req_array(row, key);
    if (!side.has_value()) {
        return std::unexpected(side.error());
    }
    const auto first = used;
    for (auto entry : *side) {
        if (used == pool.size()) {
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
        pool[used] = WireLevel{.price = cells[0], .qty = cells[1]};
        ++used;
    }
    return std::span<const WireLevel>{pool.data() + first, used - first};
}

std::expected<WireStat, WireError> read_interest(json::ondemand::object& row) {
    const auto market = json::req_str(row, "market");
    const auto oi = json::opt_str(row, "openInterest");
    if (!market.has_value() || !oi.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    return WireStat{.market = *market, .first = *oi, .second = {}};
}

std::expected<WireStat, WireError> read_volume(json::ondemand::object& row) {
    const auto market = json::req_str(row, "market");
    const auto base = json::opt_str(row, "volume");
    const auto quote = json::opt_str(row, "quoteVolume");
    if (!market.has_value() || !base.has_value() || !quote.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    return WireStat{.market = *market, .first = *base, .second = *quote};
}

} // namespace

std::optional<std::uint64_t> iso_to_ns(std::string_view text) noexcept {
    unsigned year = 0;
    unsigned month = 0;
    unsigned day = 0;
    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;
    if (text.size() < 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':' ||
        text.back() != 'Z' || !digits(text, 0, 4, year) || !digits(text, 5, 2, month) || !digits(text, 8, 2, day) ||
        !digits(text, 11, 2, hour) || !digits(text, 14, 2, minute) || !digits(text, 17, 2, second) || month == 0 ||
        month > 12 || day == 0 || day > 31) {
        return std::nullopt;
    }
    std::uint64_t fraction = 0;
    const auto rest = text.substr(19, text.size() - 20);
    if (!rest.empty()) {
        if (rest.front() != '.' || rest.size() > 10 || rest.size() < 2) {
            return std::nullopt;
        }
        for (std::size_t i = 1; i < 10; ++i) {
            const char c = i < rest.size() ? rest[i] : '0';
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
            fraction = fraction * 10 + static_cast<std::uint64_t>(c - '0');
        }
    }
    const auto days = days_from_civil(year, month, day);
    return ((days * 24 + hour) * 60 + minute) * 60 * 1'000'000'000ULL + second * 1'000'000'000ULL + fraction;
}

std::optional<std::uint64_t> server_time(std::string_view body) noexcept {
    const auto at = json::guarded([&]() -> std::expected<std::uint64_t, WireError> {
        auto root = json::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto result = json::req_object(*root, "result");
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        const auto time = json::req_str(*result, "time");
        if (!time.has_value()) {
            return std::unexpected(time.error());
        }
        const auto ns = iso_to_ns(*time);
        if (!ns.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        return *ns;
    });
    return at.has_value() ? std::optional<std::uint64_t>{*at} : std::nullopt;
}

std::expected<std::span<const WireMarket>, WireError> parse_markets(std::string_view body, MarketBuffer& storage) {
    return json::guarded([&]() -> std::expected<std::span<const WireMarket>, WireError> {
        auto root = json::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto result = json::req_object(*root, "result");
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        auto perps = json::req_object(*result, "perps");
        if (!perps.has_value()) {
            return std::unexpected(perps.error());
        }
        auto rows = json::req_array(*perps, "tradingPairs");
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        return read_rows(*rows, std::span<WireMarket>{storage.markets}, read_market);
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
        if (*type == "subscribed") {
            return Frame::Subscribed;
        }
        if (*type == "pong") {
            return Frame::Pong;
        }
        if (*type == "error") {
            return Frame::Error;
        }
        if (*type != "update") {
            return Frame::Unknown;
        }
        const auto channel = json::opt_str(*root, "channel");
        if (!channel.has_value()) {
            return std::unexpected(channel.error());
        }
        if (*channel == "topOfBooksPerps") {
            return Frame::Top;
        }
        if (*channel == "depthBooksPerps") {
            return Frame::Depth;
        }
        if (*channel == "tradesPerps") {
            return Frame::Trades;
        }
        if (*channel == "markPricesPerps") {
            return Frame::Marks;
        }
        if (*channel == "fundingRatesPerps") {
            return Frame::Funding;
        }
        return Frame::Unknown;
    });
    return kind.value_or(Frame::Unknown);
}

std::expected<std::span<const WireBook>, WireError> parse_books(std::string_view frame, BookBuffer& storage) {
    std::size_t used = 0;
    return update_rows(frame,
                       std::span<WireBook>{storage.rows},
                       [&](json::ondemand::object& row, std::uint64_t) -> std::expected<WireBook, WireError> {
                           const auto market = json::req_str(row, "market");
                           const auto time = json::req_str(row, "time");
                           if (!market.has_value() || !time.has_value()) {
                               return std::unexpected(WireError::MissingField);
                           }
                           const auto asks = read_side(row, "asks", storage.levels, used);
                           if (!asks.has_value()) {
                               return std::unexpected(asks.error());
                           }
                           const auto bids = read_side(row, "bids", storage.levels, used);
                           if (!bids.has_value()) {
                               return std::unexpected(bids.error());
                           }
                           return WireBook{
                               .market = *market, .ts_ns = iso_to_ns(*time).value_or(0), .bids = *bids, .asks = *asks};
                       });
}

std::expected<std::span<const WirePrint>, WireError> parse_trades(std::string_view frame, PrintBuffer& storage) {
    return update_rows(frame,
                       std::span<WirePrint>{storage.prints},
                       [](json::ondemand::object& row, std::uint64_t) -> std::expected<WirePrint, WireError> {
                           const auto market = json::req_str(row, "market");
                           const auto price = json::req_str(row, "price");
                           const auto size = json::req_str(row, "size");
                           const auto side = json::req_str(row, "aggressor_side");
                           const auto time = json::req_str(row, "time");
                           if (!market.has_value() || !price.has_value() || !size.has_value() || !side.has_value() ||
                               !time.has_value()) {
                               return std::unexpected(WireError::MissingField);
                           }
                           return WirePrint{.market = *market,
                                            .price = *price,
                                            .size = *size,
                                            .side = *side,
                                            .ts_ns = iso_to_ns(*time).value_or(0)};
                       });
}

std::expected<std::span<const WireMark>, WireError> parse_marks(std::string_view frame, MarkBuffer& storage) {
    return update_rows(
        frame,
        std::span<WireMark>{storage.rows},
        [](json::ondemand::object& row, std::uint64_t) -> std::expected<WireMark, WireError> {
            const auto market = json::req_str(row, "market");
            const auto mark = json::req_str(row, "markPrice");
            const auto oracle = json::opt_str(row, "oraclePrice");
            const auto time = json::opt_str(row, "lastUpdatedTime");
            if (!market.has_value() || !mark.has_value() || !oracle.has_value() || !time.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            return WireMark{.market = *market, .mark = *mark, .oracle = *oracle, .ts_ns = iso_to_ns(*time).value_or(0)};
        });
}

std::expected<std::span<const WireFunding>, WireError> parse_funding(std::string_view frame, FundingBuffer& storage) {
    return update_rows(frame,
                       std::span<WireFunding>{storage.rows},
                       [](json::ondemand::object& row, std::uint64_t at) -> std::expected<WireFunding, WireError> {
                           const auto market = json::req_str(row, "market");
                           const auto rate = json::req_str(row, "rate");
                           const auto ends = json::opt_str(row, "intervalEnds");
                           if (!market.has_value() || !rate.has_value() || !ends.has_value()) {
                               return std::unexpected(WireError::MissingField);
                           }
                           return WireFunding{
                               .market = *market, .rate = *rate, .ends_ns = iso_to_ns(*ends).value_or(0), .ts_ns = at};
                       });
}

std::expected<std::span<const WireStat>, WireError> parse_open_interest(std::string_view body, StatBuffer& storage) {
    return result_rows(body, std::span<WireStat>{storage.rows}, read_interest);
}

std::expected<std::span<const WireStat>, WireError> parse_volumes(std::string_view body, StatBuffer& storage) {
    return result_rows(body, std::span<WireStat>{storage.rows}, read_volume);
}

} // namespace raikiri::venues::ondoperps
