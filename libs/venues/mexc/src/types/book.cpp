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

std::expected<std::size_t, WireError> read_levels(detail::ondemand::value side,
                                                  std::span<WireLevel> into) {
    detail::ondemand::array array;
    if (const auto ec = side.get_array().get(array); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    std::size_t count = 0;
    for (auto entry : array) {
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

std::expected<std::uint64_t, WireError> ms_of(detail::ondemand::value value) {
    const auto text = scalar_of(value);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    return detail::digits_to_u64(*text);
}

// One forward walk over `data`: begin and end are absent on a snapshot, and
// looking each key up by name would skip both ladders once per miss.
std::expected<void, WireError>
read_depth_data(detail::ondemand::object& data, LevelBuffer& storage, WireDepth& out) {
    std::optional<std::size_t> bids;
    std::optional<std::size_t> asks;
    std::expected<std::int64_t, WireError> begin{0};
    std::expected<std::int64_t, WireError> end{0};
    std::expected<std::int64_t, WireError> version{0};
    std::expected<std::uint64_t, WireError> cts{detail::digits_to_u64({})};
    std::expected<std::uint64_t, WireError> timestamp{detail::digits_to_u64({})};
    const auto int_of =
        [](detail::ondemand::value value) -> std::expected<std::int64_t, WireError> {
        std::int64_t n = 0;
        if (const auto ec = value.get_int64().get(n); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        return n;
    };
    for (auto field : data) {
        std::string_view key;
        if (const auto ec = field.escaped_key().get(key); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        detail::ondemand::value value;
        if (const auto ec = field.value().get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        if (key == "bids" || key == "asks") {
            const bool bid = key == "bids";
            const auto count = read_levels(value, bid ? storage.bids : storage.asks);
            if (!count.has_value()) {
                return std::unexpected(count.error());
            }
            (bid ? bids : asks) = *count;
        } else if (key == "begin") {
            begin = int_of(value);
        } else if (key == "end") {
            end = int_of(value);
        } else if (key == "version") {
            version = int_of(value);
        } else if (key == "cts") {
            cts = ms_of(value);
        } else if (key == "timestamp") {
            timestamp = ms_of(value);
        }
    }
    if (!bids.has_value() || !asks.has_value()) {
        return std::unexpected(detail::map_error(simdjson::NO_SUCH_FIELD));
    }
    if (!begin.has_value() || !end.has_value() || !version.has_value() || !cts.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.begin = *begin;
    out.end = *end;
    out.version = *version;
    out.ts_ms = *cts;
    if (out.ts_ms == 0) {
        if (!timestamp.has_value()) {
            return std::unexpected(timestamp.error());
        }
        out.ts_ms = *timestamp;
    }
    out.bids = {storage.bids.data(), *bids};
    out.asks = {storage.asks.data(), *asks};
    return {};
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
        if (const auto read = read_depth_data(*data, storage, out); !read.has_value()) {
            return std::unexpected(read.error());
        }
        const auto channel = opt_scalar(*root, "channel");
        if (!channel.has_value()) {
            return std::unexpected(channel.error());
        }
        out.snapshot = channel->empty() || *channel == "push.depth.full";
        return out;
    });
}

std::expected<DepthEvent, WireError> parse_event_and_depth(std::string_view frame,
                                                           LevelBuffer& storage) {
    return guarded([&]() -> std::expected<DepthEvent, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto symbol = envelope_symbol(*root);
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        const auto channel = req_scalar(*root, "channel");
        if (!channel.has_value()) {
            return std::unexpected(channel.error());
        }
        DepthEvent out{.event = {.kind = kind_from_channel(*channel), .symbol = *symbol},
                       .depth = std::nullopt};
        if (out.event.kind != EventKind::DepthFull || root->reset().error() != simdjson::SUCCESS) {
            return out;
        }
        auto data = detail::req_object(*root, "data");
        if (!data.has_value()) {
            return out;
        }
        WireDepth depth{};
        depth.symbol = *symbol;
        depth.snapshot = true;
        if (read_depth_data(*data, storage, depth).has_value()) {
            out.depth = depth;
        }
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
