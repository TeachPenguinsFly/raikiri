#include <raikiri/venues/paradex/wire.hpp>
#include <raikiri/wire/json.hpp>

#include <cstring>
#include <optional>

namespace raikiri::venues::paradex {

namespace json = raikiri::wire;

namespace {

constexpr std::uint16_t k_schema_id = 1;
constexpr std::size_t k_header_bytes = 8;
constexpr std::size_t k_group_header_bytes = 4;

class Reader {
public:
    explicit Reader(std::string_view frame) noexcept : frame_{frame} {}

    [[nodiscard]] std::size_t size() const noexcept { return frame_.size(); }

    template <typename T> [[nodiscard]] std::optional<T> at(std::size_t offset) const noexcept {
        if (offset > frame_.size() || frame_.size() - offset < sizeof(T)) {
            return std::nullopt;
        }
        T out{};
        std::memcpy(&out, frame_.data() + offset, sizeof(T));
        return out;
    }

    // a uint8 length and that many bytes
    [[nodiscard]] std::optional<std::string_view> var_string(std::size_t& offset) const noexcept {
        const auto length = at<std::uint8_t>(offset);
        if (!length.has_value() || frame_.size() - offset - 1 < *length) {
            return std::nullopt;
        }
        const auto out = frame_.substr(offset + 1, *length);
        offset += 1U + *length;
        return out;
    }

private:
    std::string_view frame_;
};

struct Block {
    Reader reader;
    std::size_t length{0};
    std::size_t end{0}; // where the repeating groups and var data start
};

// the header's block length, not the schema's, says where var data starts
std::expected<Block, WireError> open_block(std::string_view frame, Template expected, std::size_t min_length) {
    if (!is_sbe(frame) || template_of(frame) != expected) {
        return std::unexpected(WireError::UnknownMessage);
    }
    const Reader reader{frame};
    const auto length = reader.at<std::uint16_t>(0);
    if (!length.has_value() || *length < min_length || reader.size() < k_header_bytes + *length) {
        return std::unexpected(WireError::ShortArray);
    }
    return Block{.reader = reader, .length = *length, .end = k_header_bytes + *length};
}

std::int64_t field(const Block& block, std::size_t offset) noexcept {
    return block.reader.at<std::int64_t>(k_header_bytes + offset).value_or(k_null);
}

std::expected<std::size_t, WireError> read_group(const Reader& reader, std::size_t& offset, std::span<WireLevel> into) {
    const auto entry = reader.at<std::uint16_t>(offset);
    const auto count = reader.at<std::uint16_t>(offset + 2);
    if (!entry.has_value() || !count.has_value() || *entry < 2 * sizeof(std::int64_t)) {
        return std::unexpected(WireError::ShortArray);
    }
    if (*count > into.size()) {
        return std::unexpected(WireError::TooManyEntries);
    }
    offset += k_group_header_bytes;
    for (std::size_t i = 0; i < *count; ++i) {
        const auto price = reader.at<std::int64_t>(offset);
        const auto qty = reader.at<std::int64_t>(offset + sizeof(std::int64_t));
        if (!price.has_value() || !qty.has_value()) {
            return std::unexpected(WireError::ShortArray);
        }
        into[i] = WireLevel{.price = *price, .qty = *qty};
        offset += *entry;
    }
    return std::size_t{*count};
}

std::expected<std::string_view, WireError> market_after(const Block& block, std::size_t offset) {
    const auto market = block.reader.var_string(offset);
    if (!market.has_value() || market->empty()) {
        return std::unexpected(WireError::ShortArray);
    }
    return *market;
}

bool has_tag(json::ondemand::object& row, std::string_view wanted) {
    json::ondemand::array tags;
    if (json::find(row, "tags").get_array().get(tags) != simdjson::SUCCESS) {
        return false;
    }
    for (auto entry : tags) {
        json::ondemand::value value;
        if (entry.get(value) != simdjson::SUCCESS) {
            continue;
        }
        if (const auto tag = json::str_of(value); tag.has_value() && *tag == wanted) {
            return true;
        }
    }
    return false;
}

} // namespace

std::expected<std::span<const WireMarket>, WireError> parse_markets(std::string_view body, MarketBuffer& storage) {
    return json::guarded([&]() -> std::expected<std::span<const WireMarket>, WireError> {
        auto root = json::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto rows = json::req_array(*root, "results");
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        std::size_t count = 0;
        storage.listed = 0;
        for (auto entry : *rows) {
            json::ondemand::object row;
            if (const auto ec = entry.get_object().get(row); ec != simdjson::SUCCESS) {
                return std::unexpected(json::map_error(ec));
            }
            ++storage.listed;
            const auto kind = json::req_str(row, "asset_kind");
            if (!kind.has_value()) {
                return std::unexpected(kind.error());
            }
            if (*kind != "PERP" && *kind != "SPOT") {
                continue;
            }
            if (count == storage.markets.size()) {
                return std::unexpected(WireError::TooManyEntries);
            }
            const auto symbol = json::req_str(row, "symbol");
            const auto base = json::req_str(row, "base_currency");
            const auto quote = json::req_str(row, "quote_currency");
            const auto tick = json::req_str(row, "price_tick_size");
            const auto lot = json::req_str(row, "order_size_increment");
            const auto min = json::opt_str(row, "min_notional");
            const auto cap = json::opt_str(row, "max_funding_rate");
            const auto period = json::opt_u64(row, "funding_period_hours");
            if (!symbol.has_value() || !base.has_value() || !quote.has_value() || !tick.has_value() ||
                !lot.has_value() || !min.has_value() || !cap.has_value() || !period.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            storage.markets[count] = WireMarket{.symbol = *symbol,
                                                .base = *base,
                                                .quote = *quote,
                                                .kind = *kind,
                                                .tick = *tick,
                                                .lot = *lot,
                                                .min_notional = *min,
                                                .max_funding_rate = *cap,
                                                .funding_period_hours = static_cast<std::uint32_t>(*period),
                                                .rwa = has_tag(row, "RWA")};
            ++count;
        }
        return std::span<const WireMarket>{storage.markets.data(), count};
    });
}

std::expected<WireReply, WireError> parse_reply(std::string_view frame) {
    return json::guarded([&]() -> std::expected<WireReply, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireReply out{};
        const auto id = json::opt_u64(*root, "id");
        if (!id.has_value()) {
            return std::unexpected(id.error());
        }
        out.id = *id;
        json::ondemand::object error;
        if (json::find(*root, "error").get_object().get(error) == simdjson::SUCCESS) {
            const auto code = json::opt_i64(error, "code");
            out.code = code.value_or(-1);
            if (out.code == 0) {
                out.code = -1;
            }
            return out;
        }
        auto result = json::req_object(*root, "result");
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        const auto channel = json::opt_str(*result, "channel");
        if (!channel.has_value()) {
            return std::unexpected(channel.error());
        }
        out.channel = *channel;
        return out;
    });
}

bool is_sbe(std::string_view frame) noexcept {
    const Reader reader{frame};
    const auto schema = reader.at<std::uint16_t>(4);
    return frame.size() >= k_header_bytes && schema.has_value() && *schema == k_schema_id;
}

Template template_of(std::string_view frame) noexcept {
    const auto id = Reader{frame}.at<std::uint16_t>(2);
    if (!id.has_value() || *id < 1 || *id > 5) {
        return Template::Unknown;
    }
    return static_cast<Template>(*id);
}

std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage) {
    const auto block = open_block(frame, Template::Book, 17);
    if (!block.has_value()) {
        return std::unexpected(block.error());
    }
    const auto pkg = block->reader.at<std::uint8_t>(k_header_bytes + 16);
    if (!pkg.has_value() || *pkg > 1) {
        return std::unexpected(WireError::WrongType);
    }
    WireBook out{};
    out.ts_us = field(*block, 0);
    out.seq = field(*block, 8);
    out.snapshot = *pkg == 0;
    auto offset = block->end;
    const auto bids = read_group(block->reader, offset, storage.bids);
    if (!bids.has_value()) {
        return std::unexpected(bids.error());
    }
    const auto asks = read_group(block->reader, offset, storage.asks);
    if (!asks.has_value()) {
        return std::unexpected(asks.error());
    }
    const auto market = market_after(*block, offset);
    if (!market.has_value()) {
        return std::unexpected(market.error());
    }
    out.bids = std::span<const WireLevel>{storage.bids.data(), *bids};
    out.asks = std::span<const WireLevel>{storage.asks.data(), *asks};
    out.market = *market;
    return out;
}

std::expected<WireBbo, WireError> parse_bbo(std::string_view frame) {
    const auto block = open_block(frame, Template::Bbo, 48);
    if (!block.has_value()) {
        return std::unexpected(block.error());
    }
    const auto market = market_after(*block, block->end);
    if (!market.has_value()) {
        return std::unexpected(market.error());
    }
    return WireBbo{.market = *market,
                   .ts_us = field(*block, 0),
                   .seq = field(*block, 8),
                   .bid = field(*block, 16),
                   .bid_qty = field(*block, 24),
                   .ask = field(*block, 32),
                   .ask_qty = field(*block, 40)};
}

std::expected<WirePrint, WireError> parse_trade(std::string_view frame) {
    const auto block = open_block(frame, Template::Trade, 50);
    if (!block.has_value()) {
        return std::unexpected(block.error());
    }
    const auto market = market_after(*block, block->end);
    if (!market.has_value()) {
        return std::unexpected(market.error());
    }
    return WirePrint{.market = *market,
                     .ts_us = field(*block, 0),
                     .trade_id = field(*block, 16),
                     .side = block->reader.at<std::uint8_t>(k_header_bytes + 24).value_or(0),
                     .price = field(*block, 25),
                     .qty = field(*block, 33),
                     .created_us = field(*block, 41),
                     .type = block->reader.at<std::uint8_t>(k_header_bytes + 49).value_or(0)};
}

std::expected<WireSummary, WireError> parse_summary(std::string_view frame) {
    const auto block = open_block(frame, Template::Summary, 64);
    if (!block.has_value()) {
        return std::unexpected(block.error());
    }
    const auto market = market_after(*block, block->end);
    if (!market.has_value()) {
        return std::unexpected(market.error());
    }
    return WireSummary{.market = *market,
                       .ts_us = field(*block, 0),
                       .mark = field(*block, 16),
                       .index = field(*block, 24),
                       .volume_usd = field(*block, 40),
                       .open_interest = field(*block, 48),
                       .funding_rate = field(*block, 56)};
}

std::expected<WireFunding, WireError> parse_funding(std::string_view frame) {
    const auto block = open_block(frame, Template::Funding, 58);
    if (!block.has_value()) {
        return std::unexpected(block.error());
    }
    const auto market = market_after(*block, block->end);
    if (!market.has_value()) {
        return std::unexpected(market.error());
    }
    return WireFunding{.market = *market,
                       .ts_us = field(*block, 0),
                       .rate = field(*block, 16),
                       .index = field(*block, 24),
                       .created_us = field(*block, 32),
                       .premium = field(*block, 40),
                       .period_hours = block->reader.at<std::int16_t>(k_header_bytes + 56).value_or(0)};
}

} // namespace raikiri::venues::paradex
