#include <raikiri/venues/edgex/wire.hpp>
#include <raikiri/wire/json.hpp>

#include <charconv>
#include <cstring>
#include <utility>

namespace raikiri::venues::edgex {

namespace json = raikiri::wire;

namespace {

using json::guarded;
using json::session;

std::uint32_t id_of(std::string_view digits) noexcept {
    std::uint32_t out = 0;
    const auto* end = digits.data() + digits.size();
    const auto [stop, ec] = std::from_chars(digits.data(), end, out);
    return ec == std::errc{} && stop == end ? out : 0;
}

std::expected<std::uint32_t, WireError> req_id(json::ondemand::object& obj, std::string_view key) {
    const auto text = json::req_str(obj, key);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    const auto id = id_of(*text);
    if (id == 0) {
        return std::unexpected(WireError::WrongType);
    }
    return id;
}

template <typename Entry> std::expected<json::ondemand::object, WireError> as_object(Entry&& entry) {
    json::ondemand::object obj;
    if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
        return std::unexpected(json::map_error(ec));
    }
    return obj;
}

struct Coin {
    std::string_view id;
    std::string_view name;
};

inline constexpr std::size_t k_max_coins = 1024;

struct Coins {
    std::array<Coin, k_max_coins> rows{};
    std::size_t count{0};

    [[nodiscard]] std::string_view name_of(std::string_view id) const noexcept {
        for (std::size_t i = 0; i < count; ++i) {
            if (rows[i].id == id) {
                return rows[i].name;
            }
        }
        return {};
    }
};

std::expected<void, WireError> read_coins(json::ondemand::object& data, Coins& coins) {
    auto list = json::req_array(data, "coinList");
    if (!list.has_value()) {
        return std::unexpected(list.error());
    }
    for (auto entry : *list) {
        if (coins.count == k_max_coins) {
            return std::unexpected(WireError::TooManyEntries);
        }
        auto obj = as_object(entry);
        if (!obj.has_value()) {
            return std::unexpected(obj.error());
        }
        const auto id = json::req_str(*obj, "coinId");
        const auto name = json::req_str(*obj, "coinName");
        if (!id.has_value() || !name.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        coins.rows[coins.count++] = {.id = *id, .name = *name};
    }
    return {};
}

struct RowKeys {
    std::string_view list;
    std::string_view id;
    std::string_view name;
    bool perp{false};
};

// one unreadable row fails the whole parse rather than delisting it silently
std::expected<std::span<const WireContract>, WireError>
parse_catalogue(std::string_view body, ContractBuffer& storage, const RowKeys& keys) {
    return guarded([&]() -> std::expected<std::span<const WireContract>, WireError> {
        static thread_local Coins coins;
        coins.count = 0;
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto code = json::req_str(*root, "code");
        if (!code.has_value() || *code != "SUCCESS") {
            return std::unexpected(WireError::UnknownMessage);
        }
        auto data = json::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        if (const auto read = read_coins(*data, coins); !read.has_value()) {
            return std::unexpected(read.error());
        }
        auto list = json::req_array(*data, keys.list);
        if (!list.has_value()) {
            return std::unexpected(list.error());
        }
        std::size_t count = 0;
        for (auto entry : *list) {
            if (count == k_max_contracts) {
                return std::unexpected(WireError::TooManyEntries);
            }
            auto obj = as_object(entry);
            if (!obj.has_value()) {
                return std::unexpected(obj.error());
            }
            WireContract row{};
            const auto id = req_id(*obj, keys.id);
            const auto name = json::req_str(*obj, keys.name);
            const auto base = json::req_str(*obj, "baseCoinId");
            const auto quote = json::req_str(*obj, "quoteCoinId");
            const auto tick = json::req_str(*obj, "tickSize");
            const auto step = json::req_str(*obj, "stepSize");
            const auto enabled = json::opt_bool(*obj, "enableTrade");
            if (!id.has_value() || !name.has_value() || !base.has_value() || !quote.has_value() || !tick.has_value() ||
                !step.has_value() || !enabled.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            row.id = *id;
            row.name = *name;
            row.base = coins.name_of(*base);
            row.quote = coins.name_of(*quote);
            if (row.base.empty() || row.quote.empty()) {
                return std::unexpected(WireError::MissingField);
            }
            row.tick_size = *tick;
            row.step_size = *step;
            row.enabled = *enabled;
            if (keys.perp) {
                const auto interval = json::opt_str(*obj, "fundingRateIntervalMin");
                const auto max = json::opt_str(*obj, "fundingMaxRate");
                const auto min = json::opt_str(*obj, "fundingMinRate");
                if (!interval.has_value() || !max.has_value() || !min.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                row.funding_interval_min = id_of(*interval);
                row.funding_max = *max;
                row.funding_min = *min;
            } else {
                const auto floor = json::opt_str(*obj, "minOrderValue");
                if (!floor.has_value()) {
                    return std::unexpected(floor.error());
                }
                row.min_order_value = *floor;
            }
            storage.rows[count++] = row;
        }
        return std::span<const WireContract>{storage.rows.data(), count};
    });
}

std::expected<json::ondemand::array, WireError> open_data(std::string_view frame, bool* snapshot = nullptr) {
    auto root = session().open_object(frame);
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    auto content = json::req_object(*root, "content");
    if (!content.has_value()) {
        return std::unexpected(content.error());
    }
    if (snapshot != nullptr) {
        const auto type = json::req_str(*content, "dataType");
        if (!type.has_value()) {
            return std::unexpected(type.error());
        }
        *snapshot = *type == "Snapshot";
    }
    return json::req_array(*content, "data");
}

std::expected<std::size_t, WireError>
read_levels(json::ondemand::object& row, std::string_view key, std::span<WireLevel> into) {
    auto array = json::req_array(row, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto entry : *array) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        auto obj = as_object(entry);
        if (!obj.has_value()) {
            return std::unexpected(obj.error());
        }
        const auto price = json::req_str(*obj, "price");
        const auto size = json::req_str(*obj, "size");
        if (!price.has_value() || !size.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        into[count++] = {.price = *price, .size = *size};
    }
    return count;
}

std::expected<std::uint64_t, WireError> req_version(json::ondemand::object& row, std::string_view key) {
    const auto text = json::req_str(row, key);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    const auto version = json::digits_to_u64(*text);
    if (version == 0) {
        return std::unexpected(WireError::WrongType);
    }
    return version;
}

// {\"type\": \"subscribe\", \"channel\": \"depth.99999999.15\"}, still escaped.
std::string_view channel_in(std::string_view request) noexcept {
    constexpr std::string_view key = "channel";
    const auto at = request.find(key);
    if (at == std::string_view::npos) {
        return {};
    }
    auto rest = request.substr(at + key.size());
    const auto begin = rest.find_first_not_of("\\\": ");
    if (begin == std::string_view::npos) {
        return {};
    }
    rest = rest.substr(begin);
    return rest.substr(0, rest.find_first_of("\\\""));
}

std::string_view channel_for(exec::SubKind kind) noexcept {
    switch (kind) {
    case exec::SubKind::Bbo:
    case exec::SubKind::Depth:
        return "depth";
    case exec::SubKind::Trades:
        return "trades";
    case exec::SubKind::Funding:
    case exec::SubKind::MarkPrice:
    case exec::SubKind::Kline:
        return {};
    }
    return {};
}

} // namespace

std::expected<std::span<const WireContract>, WireError> parse_perp_catalogue(std::string_view body,
                                                                             ContractBuffer& storage) {
    return parse_catalogue(
        body, storage, {.list = "contractList", .id = "contractId", .name = "contractName", .perp = true});
}

std::expected<std::span<const WireContract>, WireError> parse_spot_catalogue(std::string_view body,
                                                                             ContractBuffer& storage) {
    return parse_catalogue(body, storage, {.list = "symbolList", .id = "symbolId", .name = "symbolName"});
}

std::expected<EventInfo, WireError> parse_event(std::string_view frame) {
    return guarded([&]() -> std::expected<EventInfo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto type = json::req_str(*root, "type");
        if (!type.has_value()) {
            return std::unexpected(type.error());
        }
        if (*type == "ping") {
            return EventInfo{.kind = EventKind::Ping};
        }
        if (*type == "error") {
            return EventInfo{.kind = EventKind::Error};
        }
        if (*type != "quote-event") {
            return EventInfo{};
        }
        const auto channel = json::req_str(*root, "channel");
        if (!channel.has_value()) {
            return std::unexpected(channel.error());
        }
        EventInfo out{};
        if (channel->starts_with(k_depth_prefix)) {
            out.kind = EventKind::Depth;
        } else if (channel->starts_with("trades.")) {
            out.kind = EventKind::Trades;
        } else if (channel->starts_with("ticker.")) {
            out.kind = EventKind::Ticker;
        } else {
            return EventInfo{};
        }
        auto content = json::req_object(*root, "content");
        if (!content.has_value()) {
            return std::unexpected(content.error());
        }
        const auto data_type = json::req_str(*content, "dataType");
        if (!data_type.has_value()) {
            return std::unexpected(data_type.error());
        }
        out.snapshot = *data_type == "Snapshot";
        return out;
    });
}

std::expected<std::string_view, WireError> parse_ping(std::string_view frame) {
    return guarded([&]() -> std::expected<std::string_view, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto time = json::req_str(*root, "time");
        if (!time.has_value()) {
            return std::unexpected(time.error());
        }
        if (json::digits_to_u64(*time) == 0) {
            return std::unexpected(WireError::WrongType);
        }
        return *time;
    });
}

std::expected<StreamError, WireError> parse_stream_error(std::string_view frame) {
    return guarded([&]() -> std::expected<StreamError, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto request = json::opt_str(*root, "request");
        auto content = json::req_object(*root, "content");
        if (!request.has_value() || !content.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        const auto code = json::req_str(*content, "code");
        const auto message = json::opt_str(*content, "msg");
        if (!code.has_value() || !message.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        return StreamError{.code = *code, .message = *message, .channel = channel_in(*request)};
    });
}

std::expected<WireDepth, WireError> parse_depth(std::string_view frame, LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireDepth, WireError> {
        WireDepth out{};
        auto data = open_data(frame, &out.snapshot);
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        std::size_t rows = 0;
        for (auto entry : *data) {
            // one row per frame; a second would be a delta this parse drops, so it is refused
            if (++rows > 1) {
                return std::unexpected(WireError::TooManyEntries);
            }
            auto row = as_object(entry);
            if (!row.has_value()) {
                return std::unexpected(row.error());
            }
            const auto contract = req_id(*row, "contractId");
            const auto start = req_version(*row, "startVersion");
            const auto end = req_version(*row, "endVersion");
            if (!contract.has_value() || !start.has_value() || !end.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            const auto bids = read_levels(*row, "bids", storage.bids);
            if (!bids.has_value()) {
                return std::unexpected(bids.error());
            }
            const auto asks = read_levels(*row, "asks", storage.asks);
            if (!asks.has_value()) {
                return std::unexpected(asks.error());
            }
            out.contract_id = *contract;
            out.start_version = *start;
            out.end_version = *end;
            out.bids = {storage.bids.data(), *bids};
            out.asks = {storage.asks.data(), *asks};
        }
        if (rows == 0) {
            return std::unexpected(WireError::MissingField);
        }
        return out;
    });
}

std::expected<std::span<const WirePrint>, WireError> parse_trades(std::string_view frame, PrintBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WirePrint>, WireError> {
        auto data = open_data(frame);
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        std::size_t count = 0;
        for (auto entry : *data) {
            if (count == k_max_prints) {
                return std::unexpected(WireError::TooManyEntries);
            }
            auto obj = as_object(entry);
            if (!obj.has_value()) {
                return std::unexpected(obj.error());
            }
            const auto contract = req_id(*obj, "contractId");
            const auto price = json::req_str(*obj, "price");
            const auto size = json::req_str(*obj, "size");
            if (!contract.has_value() || !price.has_value() || !size.has_value() ||
                !json::has_field(*obj, "isBuyerMaker")) {
                return std::unexpected(WireError::MissingField);
            }
            const auto ts = json::opt_ms(*obj, "time");
            const auto maker = json::opt_bool(*obj, "isBuyerMaker");
            if (!ts.has_value() || !maker.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            storage.prints[count++] = {
                .contract_id = *contract, .price = *price, .size = *size, .ts_ms = *ts, .buyer_maker = *maker};
        }
        return std::span<const WirePrint>{storage.prints.data(), count};
    });
}

std::expected<std::span<const WireTicker>, WireError> parse_tickers(std::string_view frame, TickerBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireTicker>, WireError> {
        auto data = open_data(frame);
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        std::size_t count = 0;
        for (auto entry : *data) {
            if (count == k_max_tickers) {
                return std::unexpected(WireError::TooManyEntries);
            }
            auto obj = as_object(entry);
            if (!obj.has_value()) {
                return std::unexpected(obj.error());
            }
            WireTicker row{};
            const auto contract = req_id(*obj, "contractId");
            if (!contract.has_value()) {
                return std::unexpected(contract.error());
            }
            row.contract_id = *contract;
            const std::array<std::pair<std::string_view, std::string_view*>, 4> fields{{
                {"markPrice", &row.mark_price},
                {"fundingRate", &row.funding_rate},
                {"size", &row.volume_base},
                {"value", &row.volume_quote},
            }};
            for (const auto& [key, into] : fields) {
                const auto value = json::opt_str(*obj, key);
                if (!value.has_value()) {
                    return std::unexpected(value.error());
                }
                *into = *value;
            }
            const auto next = json::opt_ms(*obj, "nextFundingTime");
            if (!next.has_value()) {
                return std::unexpected(next.error());
            }
            row.next_funding_ms = *next;
            storage.rows[count++] = row;
        }
        return std::span<const WireTicker>{storage.rows.data(), count};
    });
}

std::string_view channel_name(exec::SubKind kind, std::uint32_t contract_id, std::span<char> out) noexcept {
    const auto channel = channel_for(kind);
    if (channel.empty()) {
        return {};
    }
    auto* cursor = out.data();
    auto* const end = out.data() + out.size();
    const auto put = [&](std::string_view part) {
        if (static_cast<std::size_t>(end - cursor) < part.size()) {
            return false;
        }
        part.copy(cursor, part.size());
        cursor += part.size();
        return true;
    };
    const auto number = [&](std::uint32_t value) {
        const auto [stop, ec] = std::to_chars(cursor, end, value);
        cursor = stop;
        return ec == std::errc{};
    };
    if (!put(channel) || !put(".") || !number(contract_id)) {
        return {};
    }
    if (channel == "depth" && (!put(".") || !number(k_book_levels))) {
        return {};
    }
    return {out.data(), static_cast<std::size_t>(cursor - out.data())};
}

std::uint32_t depth_contract(std::string_view channel) noexcept {
    if (!channel.starts_with(k_depth_prefix)) {
        return 0;
    }
    const auto rest = channel.substr(k_depth_prefix.size());
    return id_of(rest.substr(0, rest.find('.')));
}

std::expected<std::string_view, WsBuildError> Protocol::subscribe_frame(std::string_view channel) {
    return frame("subscribe", "channel", channel);
}

std::expected<std::string_view, WsBuildError> Protocol::unsubscribe_frame(std::string_view channel) {
    return frame("unsubscribe", "channel", channel);
}

std::expected<std::string_view, WsBuildError> Protocol::pong_frame(std::string_view time) {
    return frame("pong", "time", time);
}

std::expected<std::string_view, WsBuildError>
Protocol::frame(std::string_view type, std::string_view key, std::string_view value) {
    const std::array<std::string_view, 7> parts{R"({"type":")", type, R"(",")", key, R"(":")", value, R"("})"};
    std::size_t size = 0;
    for (const auto part : parts) {
        size += part.size();
    }
    if (size > frame_.size()) {
        return std::unexpected(WsBuildError::BufferTooSmall);
    }
    auto* cursor = frame_.data();
    for (const auto part : parts) {
        part.copy(cursor, part.size());
        cursor += part.size();
    }
    return std::string_view{frame_.data(), size};
}

} // namespace raikiri::venues::edgex
