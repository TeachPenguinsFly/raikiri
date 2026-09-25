#include <raikiri/venues/dydx/wire.hpp>
#include <raikiri/wire/json.hpp>

#include <cstring>
#include <initializer_list>

namespace raikiri::venues::dydx {

namespace {

using wire::guarded;
using wire::session;

std::expected<WireMarket, WireError> read_market(wire::ondemand::object& obj) {
    WireMarket out{};
    const auto ticker = wire::req_str(obj, "ticker");
    const auto status = wire::req_str(obj, "status");
    const auto tick = wire::req_str(obj, "tickSize");
    const auto step = wire::req_str(obj, "stepSize");
    if (!ticker.has_value() || !status.has_value() || !tick.has_value() || !step.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    out.ticker = *ticker;
    out.status = *status;
    out.tick_size = *tick;
    out.step_size = *step;
    // A zero read for an absent exponent would be a grid a hundred-fold off, not a missing one.
    for (const auto key : {"atomicResolution", "quantumConversionExponent", "stepBaseQuantums", "subticksPerTick"}) {
        if (!wire::has_field(obj, key)) {
            return std::unexpected(WireError::MissingField);
        }
    }
    const auto resolution = wire::opt_i64(obj, "atomicResolution");
    const auto conversion = wire::opt_i64(obj, "quantumConversionExponent");
    const auto quantums = wire::opt_u64(obj, "stepBaseQuantums");
    const auto subticks = wire::opt_u64(obj, "subticksPerTick");
    if (!resolution.has_value() || !conversion.has_value() || !quantums.has_value() || !subticks.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.atomic_resolution = *resolution;
    out.quantum_conversion_exponent = *conversion;
    out.step_base_quantums = *quantums;
    out.subticks_per_tick = *subticks;
    const auto oracle = wire::opt_str(obj, "oraclePrice");
    const auto funding = wire::opt_str(obj, "nextFundingRate");
    const auto volume = wire::opt_str(obj, "volume24H");
    if (!oracle.has_value() || !funding.has_value() || !volume.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.oracle_price = *oracle;
    out.next_funding_rate = *funding;
    out.volume_24h = *volume;
    return out;
}

// Both shapes: {"price":"1","size":"2"} on a snapshot, ["1","2"] on a delta.
std::expected<WireLevel, WireError> read_level(wire::ondemand::value entry) {
    wire::ondemand::json_type type{};
    if (const auto ec = entry.type().get(type); ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    if (type == wire::ondemand::json_type::object) {
        wire::ondemand::object obj;
        if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        const auto price = wire::req_str(obj, "price");
        const auto size = wire::req_str(obj, "size");
        if (!price.has_value() || !size.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        return WireLevel{.price = *price, .size = *size};
    }
    wire::ondemand::array pair;
    if (const auto ec = entry.get_array().get(pair); ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    WireLevel out{};
    std::size_t field = 0;
    for (auto element : pair) {
        wire::ondemand::value value;
        if (const auto ec = element.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        const auto text = wire::str_of(value);
        if (!text.has_value()) {
            return std::unexpected(text.error());
        }
        if (field == 0) {
            out.price = *text;
        } else if (field == 1) {
            out.size = *text;
        }
        ++field;
    }
    if (field < 2) {
        return std::unexpected(WireError::ShortArray);
    }
    return out;
}

struct Cursor {
    std::size_t bids{0};
    std::size_t asks{0};
    std::size_t prints{0};
    bool overflow{false};
};

std::expected<void, WireError> read_side(
    wire::ondemand::object& obj, std::string_view key, std::span<WireLevel> into, std::size_t& count, bool& overflow) {
    if (!wire::has_field(obj, key)) {
        return {};
    }
    auto array = wire::req_array(obj, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    for (auto entry : *array) {
        if (count == into.size()) {
            overflow = true;
            return {};
        }
        wire::ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        const auto level = read_level(value);
        if (!level.has_value()) {
            return std::unexpected(level.error());
        }
        into[count++] = *level;
    }
    return {};
}

std::expected<void, WireError> read_prints(wire::ondemand::object& obj, FrameBuffer& storage, Cursor& at) {
    auto array = wire::req_array(obj, "trades");
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    for (auto entry : *array) {
        if (at.prints == storage.prints.size()) {
            at.overflow = true;
            return {};
        }
        wire::ondemand::object trade;
        if (const auto ec = entry.get_object().get(trade); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        const auto price = wire::req_str(trade, "price");
        const auto size = wire::req_str(trade, "size");
        const auto side = wire::req_str(trade, "side");
        const auto created = wire::opt_str(trade, "createdAt");
        if (!price.has_value() || !size.has_value() || !side.has_value() || !created.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        storage.prints[at.prints++] = WirePrint{.price = *price, .size = *size, .side = *side, .created_at = *created};
    }
    return {};
}

std::expected<void, WireError>
read_contents(wire::ondemand::object& obj, Channel channel, FrameBuffer& storage, Cursor& at) {
    if (channel == Channel::Trades) {
        return read_prints(obj, storage, at);
    }
    if (const auto bids = read_side(obj, "bids", storage.bids, at.bids, at.overflow);
        !bids.has_value() || at.overflow) {
        return bids;
    }
    return read_side(obj, "asks", storage.asks, at.asks, at.overflow);
}

FrameType frame_type(std::string_view type) noexcept {
    if (type == "channel_batch_data" || type == "channel_data") {
        return FrameType::Data;
    }
    if (type == "subscribed") {
        return FrameType::Subscribed;
    }
    if (type == "unsubscribed") {
        return FrameType::Unsubscribed;
    }
    if (type == "error") {
        return FrameType::Error;
    }
    if (type == "connected") {
        return FrameType::Connected;
    }
    return FrameType::Unknown;
}

Channel channel_of(std::string_view name) noexcept {
    if (name == k_book_channel) {
        return Channel::Book;
    }
    if (name == k_trades_channel) {
        return Channel::Trades;
    }
    return Channel::Other;
}

} // namespace

std::expected<std::span<const WireMarket>, WireError> parse_perpetual_markets(std::string_view body,
                                                                              MarketBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireMarket>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto markets = wire::req_object(*root, "markets");
        if (!markets.has_value()) {
            return std::unexpected(markets.error());
        }
        std::size_t count = 0;
        for (auto entry : *markets) {
            if (count == k_max_markets) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::field field;
            if (const auto ec = std::move(entry).get(field); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            wire::ondemand::object obj;
            if (const auto ec = field.value().get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            const auto row = read_market(obj);
            if (!row.has_value()) {
                return std::unexpected(row.error());
            }
            storage.rows[count++] = *row;
        }
        return std::span<const WireMarket>{storage.rows.data(), count};
    });
}

std::expected<WireFrame, WireError> parse_frame(std::string_view frame, FrameBuffer& storage) {
    return guarded([&]() -> std::expected<WireFrame, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto type = wire::req_str(*root, "type");
        if (!type.has_value()) {
            return std::unexpected(type.error());
        }
        WireFrame out{};
        out.type = frame_type(*type);
        const auto channel = wire::opt_str(*root, "channel");
        const auto id = wire::opt_str(*root, "id");
        const auto message = wire::opt_str(*root, "message");
        if (!channel.has_value() || !id.has_value() || !message.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.channel = channel_of(*channel);
        out.id = *id;
        out.message = *message;
        const bool carries = out.channel != Channel::Other &&
                             (out.type == FrameType::Data ||
                              (out.type == FrameType::Subscribed && out.channel == Channel::Book));
        if (!carries) {
            return out;
        }
        wire::ondemand::value contents;
        if (const auto ec = wire::find(*root, "contents").get(contents); ec != simdjson::SUCCESS) {
            return std::unexpected(WireError::MissingField);
        }
        Cursor at{};
        wire::ondemand::json_type kind{};
        if (const auto ec = contents.type().get(kind); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        if (kind == wire::ondemand::json_type::array) {
            wire::ondemand::array batch;
            if (const auto ec = contents.get_array().get(batch); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            for (auto entry : batch) {
                wire::ondemand::object obj;
                if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                    return std::unexpected(wire::map_error(ec));
                }
                if (const auto read = read_contents(obj, out.channel, storage, at); !read.has_value()) {
                    return std::unexpected(read.error());
                }
                if (at.overflow) {
                    break;
                }
            }
        } else {
            wire::ondemand::object obj;
            if (const auto ec = contents.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            if (const auto read = read_contents(obj, out.channel, storage, at); !read.has_value()) {
                return std::unexpected(read.error());
            }
        }
        out.bids = {storage.bids.data(), at.bids};
        out.asks = {storage.asks.data(), at.asks};
        out.prints = {storage.prints.data(), at.prints};
        out.overflow = at.overflow;
        return out;
    });
}

Channel refused_channel(std::string_view message) noexcept {
    if (message.find(k_book_channel) != std::string_view::npos) {
        return Channel::Book;
    }
    if (message.find(k_trades_channel) != std::string_view::npos) {
        return Channel::Trades;
    }
    return Channel::Other;
}

std::string_view channel_name(Channel channel) noexcept {
    switch (channel) {
    case Channel::Book:
        return k_book_channel;
    case Channel::Trades:
        return k_trades_channel;
    case Channel::Other:
        break;
    }
    return {};
}

namespace {

std::string_view
build(std::span<char> buf, std::string_view type, Channel channel, std::string_view ticker, bool batched) {
    const auto name = channel_name(channel);
    if (name.empty()) {
        return {};
    }
    std::size_t at = 0;
    bool fits = true;
    const auto put = [&](std::string_view part) {
        if (at + part.size() > buf.size()) {
            fits = false;
            return;
        }
        std::memcpy(buf.data() + at, part.data(), part.size());
        at += part.size();
    };
    put(R"({"type":")");
    put(type);
    put(R"(","channel":")");
    put(name);
    put(R"(","id":")");
    put(ticker);
    put(batched ? R"(","batched":true})" : R"("})");
    return fits ? std::string_view{buf.data(), at} : std::string_view{};
}

} // namespace

// books batch; trades are rare enough to take one by one
std::string_view Protocol::subscribe(Channel channel, std::string_view ticker) {
    return build(frame_, "subscribe", channel, ticker, channel == Channel::Book);
}

std::string_view Protocol::unsubscribe(Channel channel, std::string_view ticker) {
    return build(frame_, "unsubscribe", channel, ticker, false);
}

} // namespace raikiri::venues::dydx
