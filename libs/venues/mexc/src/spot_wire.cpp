#include <raikiri/venues/mexc/spot_wire.hpp>

#include <raikiri/wire/protobuf.hpp>

namespace raikiri::venues::mexc {

namespace {

namespace pb = raikiri::wire::pb;

// Envelope fields, off a live capture.
constexpr std::uint32_t k_field_channel = 1;
constexpr std::uint32_t k_field_symbol = 3;
constexpr std::uint32_t k_field_send_ms = 6;

// Payload fields. Which one is present IS the frame's type — the channel
// string names it too, but the number is what the decoder can act on.
constexpr std::uint32_t k_field_depth = 303;
constexpr std::uint32_t k_field_deals = 314;
constexpr std::uint32_t k_field_book_ticker = 315;

StreamKind kind_of(std::uint32_t number) noexcept {
    switch (number) {
    case k_field_book_ticker:
        return StreamKind::Quote;
    case k_field_deals:
        return StreamKind::Trade;
    case k_field_depth:
        return StreamKind::Depth;
    default:
        return StreamKind::Unknown;
    }
}

std::expected<WireLevel, WireError> read_level(std::string_view encoded) {
    const auto price = pb::bytes_field(encoded, 1);
    const auto qty = pb::bytes_field(encoded, 2);
    if (!price.has_value() || !qty.has_value() || price->empty() || qty->empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return WireLevel{.price = *price, .qty = *qty};
}

std::expected<std::size_t, WireError> read_side(std::string_view payload,
                                                std::uint32_t number,
                                                std::span<WireLevel> into) {
    std::size_t count = 0;
    pb::Reader reader{payload};
    while (const auto field = reader.next()) {
        if (field->number != number || field->type != pb::WireType::Bytes) {
            continue;
        }
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        const auto level = read_level(field->bytes);
        if (!level.has_value()) {
            return std::unexpected(level.error());
        }
        into[count] = *level;
        ++count;
    }
    return count;
}

} // namespace

std::expected<StreamEvent, WireError> parse_stream_event(std::string_view frame) {
    StreamEvent out{};
    bool named = false;
    pb::Reader reader{frame};
    while (const auto field = reader.next()) {
        if (field->type == pb::WireType::Varint) {
            if (field->number == k_field_send_ms) {
                out.ts_ms = field->varint;
            }
            continue;
        }
        if (field->type != pb::WireType::Bytes) {
            continue;
        }
        switch (field->number) {
        case k_field_channel:
            named = true;
            break;
        case k_field_symbol:
            out.symbol = field->bytes;
            break;
        default: {
            const auto kind = kind_of(field->number);
            if (kind != StreamKind::Unknown) {
                out.kind = kind;
                out.payload = field->bytes;
            }
            break;
        }
        }
    }
    if (!named || out.symbol.empty()) {
        // Every data frame names its channel and its symbol. Something that
        // does neither is a control frame or not a message at all.
        return std::unexpected(WireError::MissingField);
    }
    return out;
}

std::expected<StreamQuote, WireError> parse_stream_quote(std::string_view payload) {
    const auto bid = pb::bytes_field(payload, 1);
    const auto bid_qty = pb::bytes_field(payload, 2);
    const auto ask = pb::bytes_field(payload, 3);
    const auto ask_qty = pb::bytes_field(payload, 4);
    if (!bid.has_value() || !bid_qty.has_value() || !ask.has_value() || !ask_qty.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    if (bid->empty() || bid_qty->empty() || ask->empty() || ask_qty->empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return StreamQuote{
        .bid_price = *bid, .bid_qty = *bid_qty, .ask_price = *ask, .ask_qty = *ask_qty};
}

std::expected<std::span<const StreamTrade>, WireError>
parse_stream_trades(std::string_view payload, StreamTradeBuffer& storage) {
    std::size_t count = 0;
    pb::Reader reader{payload};
    while (const auto field = reader.next()) {
        if (field->number != 1 || field->type != pb::WireType::Bytes) {
            continue;
        }
        if (count == storage.trades.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        const auto price = pb::bytes_field(field->bytes, 1);
        const auto qty = pb::bytes_field(field->bytes, 2);
        const auto trade_type = pb::varint_field(field->bytes, 3);
        const auto ts = pb::varint_field(field->bytes, 4);
        const auto id = pb::bytes_field(field->bytes, 5);
        if (!price.has_value() || !qty.has_value() || !trade_type.has_value() || !ts.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        storage.trades[count] = StreamTrade{.trade_id = id.value_or(std::string_view{}),
                                          .price = *price,
                                          .qty = *qty,
                                          .aggressor_bought = *trade_type == 1,
                                          .ts_ms = *ts};
        ++count;
    }
    if (count == 0) {
        return std::unexpected(WireError::MissingField);
    }
    return std::span<const StreamTrade>{storage.trades.data(), count};
}

std::expected<StreamBook, WireError> parse_stream_book(std::string_view payload,
                                                   LevelBuffer& storage) {
    const auto asks = read_side(payload, 1, storage.asks);
    if (!asks.has_value()) {
        return std::unexpected(asks.error());
    }
    const auto bids = read_side(payload, 2, storage.bids);
    if (!bids.has_value()) {
        return std::unexpected(bids.error());
    }
    if (*asks == 0 && *bids == 0) {
        return std::unexpected(WireError::MissingField);
    }
    return StreamBook{.bids = {storage.bids.data(), *bids}, .asks = {storage.asks.data(), *asks}};
}

} // namespace raikiri::venues::mexc
