#include <raikiri/venues/mexc/book.hpp>
#include <raikiri/venues/mexc/envelope.hpp>
#include <raikiri/wire/json.hpp>
#include <raikiri/wire/protobuf.hpp>

namespace raikiri::venues::mexc {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson layer;
// a venue that reimplements opt_str or req_array is one that will disagree
// with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

namespace pb = raikiri::wire::pb;

using detail::guarded;
using detail::session;

// A level is [64150.5,65907,7]: price, size in CONTRACTS, and the resting
// order count. Only the first two are read.
std::expected<WireLevel, WireError> read_level(detail::ondemand::value entry) {
    detail::ondemand::array triple;
    if (const auto ec = entry.get_array().get(triple); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    WireLevel level{};
    std::size_t seen = 0;
    for (auto element : triple) {
        detail::ondemand::value value;
        if (const auto ec = element.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        if (seen < 2) {
            const auto text = scalar_of(value);
            if (!text.has_value()) {
                return std::unexpected(text.error());
            }
            (seen == 0 ? level.price : level.qty) = *text;
        }
        ++seen;
    }
    if (seen < 2) {
        return std::unexpected(WireError::MissingField);
    }
    return level;
}

std::expected<std::size_t, WireError>
read_levels(detail::ondemand::object& obj, std::string_view key, std::span<WireLevel> into) {
    auto array = detail::req_array(obj, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto entry : *array) {
        if (count == k_max_levels) {
            return std::unexpected(WireError::TooManyEntries);
        }
        detail::ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        const auto level = read_level(value);
        if (!level.has_value()) {
            return std::unexpected(level.error());
        }
        into[count] = *level;
        ++count;
    }
    return count;
}

std::expected<WireLevel, WireError> read_stream_level(std::string_view encoded) {
    const auto price = pb::bytes_field(encoded, 1);
    const auto qty = pb::bytes_field(encoded, 2);
    if (!price.has_value() || !qty.has_value() || price->empty() || qty->empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return WireLevel{.price = *price, .qty = *qty};
}

std::expected<std::size_t, WireError>
read_stream_side(std::string_view payload, std::uint32_t number, std::span<WireLevel> into) {
    std::size_t count = 0;
    pb::Reader reader{payload};
    while (const auto entry = reader.next()) {
        if (entry->number != number || entry->type != pb::WireType::Bytes) {
            continue;
        }
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        const auto level = read_stream_level(entry->bytes);
        if (!level.has_value()) {
            return std::unexpected(level.error());
        }
        into[count] = *level;
        ++count;
    }
    return count;
}

} // namespace

std::expected<WireDepth, WireError> parse_depth(std::string_view frame, LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireDepth, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto symbol = envelope_symbol(*root);
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        auto data = detail::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireDepth out{};
        out.symbol = *symbol;
        const auto bids = read_levels(*data, "bids", storage.bids);
        if (!bids.has_value()) {
            return std::unexpected(bids.error());
        }
        const auto asks = read_levels(*data, "asks", storage.asks);
        if (!asks.has_value()) {
            return std::unexpected(asks.error());
        }
        // begin/end are absent on a snapshot; version is on both.
        const auto begin = opt_int(*data, "begin", 0);
        const auto end = opt_int(*data, "end", 0);
        const auto version = opt_int(*data, "version", 0);
        const auto cts = scalar_ms(*data, "cts");
        if (!begin.has_value() || !end.has_value() || !version.has_value() || !cts.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.begin = *begin;
        out.end = *end;
        out.version = *version;
        out.ts_ms = *cts;
        if (out.ts_ms == 0) {
            const auto ts = scalar_ms(*data, "timestamp");
            if (!ts.has_value()) {
                return std::unexpected(ts.error());
            }
            out.ts_ms = *ts;
        }
        out.bids = {storage.bids.data(), *bids};
        out.asks = {storage.asks.data(), *asks};
        const auto channel = opt_scalar(*root, "channel");
        if (!channel.has_value()) {
            return std::unexpected(channel.error());
        }
        out.snapshot = channel->empty() || *channel == "push.depth.full";
        return out;
    });
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

std::expected<StreamBook, WireError> parse_stream_book(std::string_view payload,
                                                       LevelBuffer& storage) {
    const auto asks = read_stream_side(payload, 1, storage.asks);
    if (!asks.has_value()) {
        return std::unexpected(asks.error());
    }
    const auto bids = read_stream_side(payload, 2, storage.bids);
    if (!bids.has_value()) {
        return std::unexpected(bids.error());
    }
    if (*asks == 0 && *bids == 0) {
        return std::unexpected(WireError::MissingField);
    }
    return StreamBook{.bids = {storage.bids.data(), *bids}, .asks = {storage.asks.data(), *asks}};
}

} // namespace raikiri::venues::mexc
