#include <raikiri/common/int128.hpp>
#include <raikiri/venues/lighter/account.hpp>
#include <raikiri/wire/json.hpp>

#include <array>

namespace raikiri::venues::lighter {

namespace {

using wire::guarded;
using wire::session;

constexpr std::size_t k_max_rows = 4096;

std::expected<WireOrder, WireError> read_order(wire::ondemand::object& obj) {
    WireOrder out{};
    const auto index = wire::opt_u64(obj, "order_index");
    const auto client = wire::opt_i64(obj, "client_order_index");
    const auto market = wire::opt_u64(obj, "market_index");
    const auto ask = wire::opt_bool(obj, "is_ask");
    const auto updated = wire::opt_u64(obj, "updated_at");
    if (!index.has_value() || !client.has_value() || !market.has_value() || !ask.has_value() || !updated.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.order_index = *index;
    out.client_order_index = *client;
    out.market_index = static_cast<std::uint32_t>(*market);
    out.is_ask = *ask;
    out.updated_ms = *updated;
    const std::array<std::pair<std::string_view, std::string_view*>, 5> strings{{
        {"status", &out.status},
        {"price", &out.price},
        {"initial_base_amount", &out.initial_base_amount},
        {"remaining_base_amount", &out.remaining_base_amount},
        {"filled_base_amount", &out.filled_base_amount},
    }};
    for (const auto& [key, into] : strings) {
        const auto text = wire::opt_str(obj, key);
        if (!text.has_value()) {
            return std::unexpected(text.error());
        }
        *into = *text;
    }
    return out;
}

std::expected<WireTrade, WireError> read_trade(wire::ondemand::object& obj) {
    WireTrade out{};
    const std::array<std::pair<std::string_view, std::uint64_t*>, 4> unsigned_fields{{
        {"trade_id", &out.trade_id},
        {"ask_id", &out.ask_id},
        {"bid_id", &out.bid_id},
        {"timestamp", &out.ts_ms},
    }};
    for (const auto& [key, into] : unsigned_fields) {
        const auto value = wire::opt_u64(obj, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *into = *value;
    }
    const std::array<std::pair<std::string_view, std::int64_t*>, 4> signed_fields{{
        {"ask_client_id", &out.ask_client_id},
        {"bid_client_id", &out.bid_client_id},
        {"ask_account_id", &out.ask_account_id},
        {"bid_account_id", &out.bid_account_id},
    }};
    for (const auto& [key, into] : signed_fields) {
        const auto value = wire::opt_i64(obj, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *into = *value;
    }
    const auto market = wire::opt_u64(obj, "market_id");
    const auto maker_ask = wire::opt_bool(obj, "is_maker_ask");
    const auto price = wire::opt_str(obj, "price");
    const auto size = wire::opt_str(obj, "size");
    if (!market.has_value() || !maker_ask.has_value() || !price.has_value() || !size.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.market_id = static_cast<std::uint32_t>(*market);
    out.is_maker_ask = *maker_ask;
    out.price = *price;
    out.size = *size;
    const auto usd = wire::opt_str(obj, "usd_amount");
    const auto taker_fee = wire::opt_u64(obj, "taker_fee");
    const auto maker_fee = wire::opt_u64(obj, "maker_fee");
    if (!usd.has_value() || !taker_fee.has_value() || !maker_fee.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.usd_amount = *usd;
    out.taker_fee = *taker_fee;
    out.maker_fee = *maker_fee;
    return out;
}

template <typename Row, typename Read>
std::expected<void, WireError> read_rows(wire::ondemand::array& rows, std::vector<Row>& out, Read&& read) {
    for (auto entry : rows) {
        if (out.size() == k_max_rows) {
            return std::unexpected(WireError::TooManyEntries);
        }
        wire::ondemand::object obj;
        if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        const auto row = read(obj);
        if (!row.has_value()) {
            return std::unexpected(row.error());
        }
        out.push_back(*row);
    }
    return {};
}

// an array of rows, or an object of arrays keyed by market id
template <typename Row, typename Read>
std::expected<void, WireError>
read_listing(std::string_view frame, std::string_view key, std::vector<Row>& out, Read&& read) {
    out.clear();
    return guarded([&]() -> std::expected<void, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto field = wire::find(*root, key);
        wire::ondemand::value value;
        if (field.get(value) != simdjson::SUCCESS) {
            return {};
        }
        wire::ondemand::json_type type{};
        if (value.type().get(type) != simdjson::SUCCESS) {
            return std::unexpected(WireError::NotJson);
        }
        if (type == wire::ondemand::json_type::array) {
            wire::ondemand::array rows = value.get_array();
            return read_rows(rows, out, read);
        }
        if (type != wire::ondemand::json_type::object) {
            return std::unexpected(WireError::WrongType);
        }
        wire::ondemand::object markets = value.get_object();
        return wire::for_each_field(markets, [&](std::string_view, wire::ondemand::value& rows_value) {
            wire::ondemand::array rows;
            if (const auto ec = rows_value.get_array().get(rows); ec != simdjson::SUCCESS) {
                return std::expected<void, WireError>{std::unexpected(wire::map_error(ec))};
            }
            return read_rows(rows, out, read);
        });
    });
}

} // namespace

std::expected<SendReply, WireError> parse_send_reply(std::string_view frame) {
    return guarded([&]() -> std::expected<SendReply, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        SendReply out{};
        const auto id = wire::opt_raw(*root, "id");
        if (!id.has_value()) {
            return std::unexpected(id.error());
        }
        out.id = *id;
        if (out.id.size() >= 2 && out.id.front() == '"') {
            out.id = out.id.substr(1, out.id.size() - 2);
        }
        wire::ondemand::object body = *root;
        wire::ondemand::object nested;
        if (wire::has_field(*root, "error")) {
            if (const auto ec = wire::find(*root, "error").get_object().get(nested); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            body = nested;
        } else if (wire::has_field(*root, "data")) {
            if (const auto ec = wire::find(*root, "data").get_object().get(nested); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            body = nested;
        }
        const auto code = wire::opt_i64(body, "code");
        const auto message = wire::opt_str(body, "message");
        const auto hash = wire::opt_str(body, "tx_hash");
        if (!code.has_value() || !message.has_value() || !hash.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.code = *code;
        out.message = *message;
        out.tx_hash = *hash;
        return out;
    });
}

std::expected<std::int64_t, WireError> parse_next_nonce(std::string_view body) {
    return guarded([&]() -> std::expected<std::int64_t, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto code = wire::opt_i64(*root, "code");
        const auto nonce = wire::opt_i64(*root, "nonce");
        if (!code.has_value() || !nonce.has_value() || *code != k_code_ok || !wire::has_field(*root, "nonce")) {
            return std::unexpected(WireError::UnknownMessage);
        }
        return *nonce;
    });
}

std::expected<void, WireError> parse_account_orders(std::string_view frame, std::vector<WireOrder>& out) {
    return read_listing(frame, "orders", out, read_order);
}

std::expected<void, WireError> parse_account_trades(std::string_view frame, std::vector<WireTrade>& out) {
    return read_listing(frame, "trades", out, read_trade);
}

std::expected<void, WireError> parse_account(std::string_view body, WireAccount& out) {
    out = WireAccount{};
    return guarded([&]() -> std::expected<void, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto accounts = wire::req_array(*root, "accounts");
        if (!accounts.has_value()) {
            return std::unexpected(accounts.error());
        }
        for (auto entry : *accounts) {
            wire::ondemand::object account;
            if (const auto ec = entry.get_object().get(account); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            const auto available = wire::opt_str(account, "available_balance");
            const auto collateral = wire::opt_str(account, "collateral");
            if (!available.has_value() || !collateral.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.available_balance = *available;
            out.collateral = *collateral;
            auto positions = wire::req_array(account, "positions");
            if (!positions.has_value()) {
                return std::unexpected(positions.error());
            }
            return read_rows(*positions, out.positions, [](wire::ondemand::object& obj) {
                WirePosition row{};
                const auto market = wire::opt_u64(obj, "market_id");
                const auto sign = wire::opt_i64(obj, "sign");
                const auto position = wire::opt_str(obj, "position");
                const auto entry_price = wire::opt_str(obj, "avg_entry_price");
                const auto unrealized = wire::opt_str(obj, "unrealized_pnl");
                if (!market.has_value() || !sign.has_value() || !position.has_value() || !entry_price.has_value() ||
                    !unrealized.has_value()) {
                    return std::expected<WirePosition, WireError>{std::unexpected(WireError::WrongType)};
                }
                row.market_id = static_cast<std::uint32_t>(*market);
                row.sign = *sign;
                row.position = *position;
                row.avg_entry_price = *entry_price;
                row.unrealized_pnl = *unrealized;
                return std::expected<WirePosition, WireError>{row};
            });
        }
        return std::unexpected(WireError::UnknownMessage);
    });
}

FixedE9 fee_of(const WireTrade& trade, bool maker) noexcept {
    const auto rate = maker ? trade.maker_fee : trade.taker_fee;
    if (rate == 0) {
        return FixedE9::from_raw(0);
    }
    const auto notional = parse_amount_e9(trade.usd_amount);
    if (!notional.has_value()) {
        return FixedE9::undef();
    }
    return FixedE9::from_raw(static_cast<std::int64_t>(static_cast<int128_t>(notional->raw) * rate / 1'000'000));
}

OrderPhase order_phase(std::string_view status) noexcept {
    if (status == "open" || status == "pending" || status == "in-progress") {
        return OrderPhase::Live;
    }
    if (status == "filled") {
        return OrderPhase::Filled;
    }
    if (status == "canceled-post-only") {
        return OrderPhase::PostOnlyCrossed;
    }
    if (status.starts_with("canceled")) {
        return OrderPhase::Canceled;
    }
    return OrderPhase::Unknown;
}

ErrorClass classify_code(std::int64_t code) noexcept {
    switch (code) {
    case 20013: // invalid auth
    case 21100: // account not found
    case 21102: // invalid account index
    case 21108: // public key not registered
    case 21109: // api key not found
    case 21110: // invalid api key index
    case 21120: // invalid signature
    case 21136: // signer too old for the venue
        return ErrorClass::AuthFailure;
    case 21506: // too many pending txs
    case 23000:
    case 23001:
    case 23003:
        return ErrorClass::RateLimited;
    case k_code_invalid_nonce:
    case 29500:
    case 29501:
        return ErrorClass::Retryable;
    default:
        return ErrorClass::Terminal;
    }
}

std::int64_t client_order_index(const ExchangeClientOrderId& id) noexcept {
    constexpr std::string_view alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    const auto text = id.view();
    std::uint64_t bits = 0;
    for (const char c : text.substr(text.size() >= 10 ? text.size() - 10 : 0)) {
        const auto at = alphabet.find(c);
        bits = (bits << 5) | (at == std::string_view::npos ? 0 : at);
    }
    const auto index = static_cast<std::int64_t>(bits & ((std::uint64_t{1} << 48) - 1));
    return index == 0 ? 1 : index;
}

} // namespace raikiri::venues::lighter
