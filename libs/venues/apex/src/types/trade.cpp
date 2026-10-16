#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/int128.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/venues/apex/trade.hpp>
#include <raikiri/wire/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>

namespace raikiri::venues::apex {

namespace json = raikiri::wire;

namespace {

using json::guarded;
using json::session;

constexpr std::uint64_t k_expiry_s = 28ULL * 86'400ULL;
constexpr std::uint64_t k_max_u32 = 0xFFFFFFFFULL;
constexpr std::uint8_t k_e9_scale = 9;
constexpr std::uint8_t k_max_decimals = 30;

[[nodiscard]] uint128_t pow10(std::uint8_t exponent) noexcept {
    uint128_t out = 1;
    for (std::uint8_t i = 0; i < exponent; ++i) {
        out *= 10;
    }
    return out;
}

[[nodiscard]] std::string_view digits(std::uint64_t value, std::span<char> out) noexcept {
    const auto [stop, ec] = std::to_chars(out.data(), out.data() + out.size(), value);
    return ec == std::errc{} ? std::string_view{out.data(), static_cast<std::size_t>(stop - out.data())}
                             : std::string_view{};
}

[[nodiscard]] std::uint64_t u64_of(std::string_view text) noexcept {
    std::uint64_t out = 0;
    const auto* end = text.data() + text.size();
    const auto [stop, ec] = std::from_chars(text.data(), end, out);
    return ec == std::errc{} && stop == end ? out : 0;
}

// standard alphabet with padding
[[nodiscard]] std::string base64(std::string_view in) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < in.size(); i += 3) {
        const auto rest = in.size() - i;
        const std::uint32_t chunk = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[i])) << 16U) |
                                    (rest > 1 ? static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[i + 1])) << 8U
                                              : 0U) |
                                    (rest > 2 ? static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[i + 2])) : 0U);
        out.push_back(alphabet[(chunk >> 18U) & 0x3FU]);
        out.push_back(alphabet[(chunk >> 12U) & 0x3FU]);
        out.push_back(rest > 1 ? alphabet[(chunk >> 6U) & 0x3FU] : '=');
        out.push_back(rest > 2 ? alphabet[chunk & 0x3FU] : '=');
    }
    return out;
}

[[nodiscard]] bool printable(std::string_view text) noexcept {
    return !text.empty() && std::ranges::all_of(text, [](char c) {
        return c > ' ' && c < 0x7F && c != '"' && c != '\\' && c != '&' && c != '=';
    });
}

void append_encoded(std::string& out, std::string_view text) {
    constexpr std::string_view hex = "0123456789ABCDEF";
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (std::isalnum(byte) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(hex[byte >> 4U]);
            out.push_back(hex[byte & 0x0FU]);
        }
    }
}

// a positive e9 amount in units of 10^-decimals, truncated as the sdk's ROUND_DOWN quantize does
[[nodiscard]] std::optional<uint128_t> scaled(FixedE9 value, std::uint8_t decimals) noexcept {
    if (value.is_undef() || value.raw <= 0 || decimals > k_max_decimals) {
        return std::nullopt;
    }
    const auto raw = static_cast<uint128_t>(value.raw);
    return decimals >= k_e9_scale ? raw * pow10(decimals - k_e9_scale) : raw / pow10(k_e9_scale - decimals);
}

// a fee rate in basis points, rounded up; the l2 field is one byte
[[nodiscard]] std::optional<std::uint8_t> fee_bps(FixedE9 rate) noexcept {
    constexpr std::int64_t k_e9_per_bp = 100'000;
    if (rate.is_undef() || rate.raw < 0) {
        return std::nullopt;
    }
    const auto bps = (rate.raw + k_e9_per_bp - 1) / k_e9_per_bp;
    return bps <= 0xFF ? std::optional{static_cast<std::uint8_t>(bps)} : std::nullopt;
}

[[nodiscard]] std::string_view fee_text(std::uint64_t e6, std::span<char> out) noexcept {
    std::array<char, 24> whole{};
    const auto integer = digits(e6 / 1'000'000ULL, whole);
    if (integer.empty() || out.size() < integer.size() + 7) {
        return {};
    }
    std::memcpy(out.data(), integer.data(), integer.size());
    out[integer.size()] = '.';
    auto frac = e6 % 1'000'000ULL;
    for (std::size_t i = 6; i > 0; --i) {
        out[integer.size() + i] = static_cast<char>('0' + (frac % 10));
        frac /= 10;
    }
    return {out.data(), integer.size() + 7};
}

void hex_into(std::span<const std::uint8_t> bytes, std::span<char> out) noexcept {
    constexpr std::string_view hex = "0123456789abcdef";
    for (std::size_t i = 0; i < bytes.size() && (2 * i) + 1 < out.size(); ++i) {
        out[2 * i] = hex[bytes[i] >> 4U];
        out[(2 * i) + 1] = hex[bytes[i] & 0x0FU];
    }
}

// strings as their contents, numbers and booleans as their raw text; an object or array gives nothing
[[nodiscard]] std::string_view text_of(json::ondemand::value& value) {
    json::ondemand::json_type type{};
    if (value.type().get(type) != simdjson::SUCCESS) {
        return {};
    }
    if (type == json::ondemand::json_type::string) {
        const auto text = json::str_of(value);
        return text.has_value() ? *text : std::string_view{};
    }
    if (type != json::ondemand::json_type::number && type != json::ondemand::json_type::boolean) {
        return {};
    }
    const auto raw = json::raw_of(value);
    return raw.has_value() ? json::trim(*raw) : std::string_view{};
}

template <typename Row, std::size_t N, typename Fill>
std::expected<std::size_t, WireError> read_rows(json::ondemand::value& value, std::array<Row, N>& into, Fill&& fill) {
    json::ondemand::array array;
    if (const auto ec = value.get_array().get(array); ec != simdjson::SUCCESS) {
        return std::unexpected(json::map_error(ec));
    }
    std::size_t count = 0;
    for (auto entry : array) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        json::ondemand::object obj;
        if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
            return std::unexpected(json::map_error(ec));
        }
        Row row{};
        const auto read = json::for_each_field(
            obj, [&](std::string_view key, json::ondemand::value& field) { fill(row, key, field); });
        if (!read.has_value()) {
            return std::unexpected(read.error());
        }
        into[count++] = row;
    }
    return count;
}

void fill_order(WireOrder& row, std::string_view key, json::ondemand::value& value) {
    if (key == "id") {
        row.id = text_of(value);
    } else if (key == "clientOrderId" || (key == "clientId" && row.client_order_id.empty())) {
        row.client_order_id = text_of(value);
    } else if (key == "symbol") {
        row.symbol = text_of(value);
    } else if (key == "side") {
        row.side = text_of(value);
    } else if (key == "price") {
        row.price = text_of(value);
    } else if (key == "size") {
        row.size = text_of(value);
    } else if (key == "status") {
        row.status = text_of(value);
    } else if (key == "cumSuccessFillSize") {
        row.cum_fill_size = text_of(value);
    } else if (key == "cancelReason") {
        row.cancel_reason = text_of(value);
    } else if (key == "updatedAt" || key == "updatedTime") {
        row.updated_ms = u64_of(text_of(value));
    }
}

void fill_fill(WireFill& row, std::string_view key, json::ondemand::value& value) {
    if (key == "id") {
        row.id = text_of(value);
    } else if (key == "orderId") {
        row.order_id = text_of(value);
    } else if (key == "clientOrderId" || (key == "clientId" && row.client_order_id.empty())) {
        row.client_order_id = text_of(value);
    } else if (key == "symbol") {
        row.symbol = text_of(value);
    } else if (key == "side") {
        row.side = text_of(value);
    } else if (key == "size") {
        row.size = text_of(value);
    } else if (key == "price") {
        row.price = text_of(value);
    } else if (key == "fee") {
        row.fee = text_of(value);
    } else if (key == "liquidity") {
        row.liquidity = text_of(value);
    } else if (key == "createdAt") {
        row.created_ms = u64_of(text_of(value));
    }
}

void fill_position(WirePosition& row, std::string_view key, json::ondemand::value& value) {
    if (key == "symbol") {
        row.symbol = text_of(value);
    } else if (key == "side") {
        row.side = text_of(value);
    } else if (key == "size") {
        row.size = text_of(value);
    } else if (key == "entryPrice") {
        row.entry_price = text_of(value);
    }
}

[[nodiscard]] std::expected<void, WireError>
read_group(std::string_view key, json::ondemand::value& value, PrivateBuffer& storage, TradeRows& out, bool fills) {
    // an account with nothing open says null, not []
    if (json::ondemand::json_type type{};
        value.type().get(type) == simdjson::SUCCESS && type == json::ondemand::json_type::null) {
        return {};
    }
    std::expected<std::size_t, WireError> count{0};
    if (key == "orders" && !fills) {
        count = read_rows(value, storage.orders, fill_order);
        if (count.has_value()) {
            out.orders = {storage.orders.data(), *count};
        }
    } else if (key == "fills" || (key == "orders" && fills)) {
        count = read_rows(value, storage.fills, fill_fill);
        if (count.has_value()) {
            out.fills = {storage.fills.data(), *count};
        }
    } else if (key == "positions") {
        count = read_rows(value, storage.positions, fill_position);
        if (count.has_value()) {
            out.positions = {storage.positions.data(), *count};
        }
    }
    if (!count.has_value()) {
        return std::unexpected(count.error());
    }
    return {};
}

// the success envelope holds data; a refusal holds code and msg instead
[[nodiscard]] std::expected<json::ondemand::value, WireError> data_of(json::ondemand::object& root) {
    json::ondemand::value data;
    if (const auto ec = json::find(root, "data").get(data); ec != simdjson::SUCCESS) {
        return std::unexpected(json::map_error(ec));
    }
    return data;
}

[[nodiscard]] std::optional<Side> side_of(std::string_view text) noexcept {
    if (text == "BUY") {
        return Side::Buy;
    }
    if (text == "SELL") {
        return Side::Sell;
    }
    return std::nullopt;
}

// the venue pads some figures to eighteen places
[[nodiscard]] std::expected<FixedE9, ParseError> decimal_e9(std::string_view text) noexcept {
    if (text.find('.') != std::string_view::npos) {
        while (text.ends_with('0')) {
            text.remove_suffix(1);
        }
        if (text.ends_with('.')) {
            text.remove_suffix(1);
        }
    }
    return parse_decimal_e9(text);
}

[[nodiscard]] std::optional<PriceTicks> ticks_of(std::string_view text, const InstrumentBase& base) noexcept {
    const auto value = decimal_e9(text);
    if (!value.has_value()) {
        return std::nullopt;
    }
    const auto ticks = base.price_to_ticks(*value);
    return ticks.has_value() ? std::optional{*ticks} : std::nullopt;
}

[[nodiscard]] std::optional<QtyLots> lots_of(std::string_view text, const InstrumentBase& base) noexcept {
    if (text.empty()) {
        return QtyLots::from_raw(0);
    }
    const auto value = decimal_e9(text);
    if (!value.has_value()) {
        return std::nullopt;
    }
    const auto lots = base.qty_to_lots(*value);
    return lots.has_value() ? std::optional{*lots} : std::nullopt;
}

[[nodiscard]] UnixNanos from_ms(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * 1'000'000ULL};
}

} // namespace

std::string_view to_string(BuildError err) noexcept {
    switch (err) {
    case BuildError::NotSigned:
        return "apex: no api key or zk seeds loaded";
    case BuildError::NoAccount:
        return "apex: account terms not loaded yet";
    case BuildError::UnknownSymbol:
        return "apex: symbol missing from the signing catalogue";
    case BuildError::BadAmount:
        return "apex: price or size outside what the l2 order can carry";
    case BuildError::BadFeeRate:
        return "apex: fee rate does not fit the l2 order";
    case BuildError::BadText:
        return "apex: client id or symbol holds characters the signature cannot carry";
    }
    return "apex: build error";
}

std::expected<SigningMeta, WireError> parse_signing_meta(std::string_view body) {
    return guarded([&]() -> std::expected<SigningMeta, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = json::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        auto config = json::req_object(*data, "contractConfig");
        if (!config.has_value()) {
            return std::unexpected(config.error());
        }
        struct Row {
            std::string_view symbol;
            std::string_view cross;
            std::string_view settle;
            std::uint32_t pair{0};
        };
        std::vector<Row> rows;
        std::unordered_map<std::string_view, std::uint8_t> decimals;
        const auto read = json::for_each_field(
            *config, [&](std::string_view key, json::ondemand::value& value) -> std::expected<void, WireError> {
                const bool contracts = key == "perpetualContract" || key == "stockContract";
                if (!contracts && key != "assets") {
                    return {};
                }
                json::ondemand::array array;
                if (const auto ec = value.get_array().get(array); ec != simdjson::SUCCESS) {
                    return std::unexpected(json::map_error(ec));
                }
                for (auto entry : array) {
                    json::ondemand::object obj;
                    if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                        return std::unexpected(json::map_error(ec));
                    }
                    Row row{};
                    std::string_view token;
                    std::uint64_t places = 0;
                    const auto fields = json::for_each_field(
                        obj, [&](std::string_view field, json::ondemand::value& v) {
                            if (field == "symbol") {
                                row.symbol = text_of(v);
                            } else if (field == "crossSymbolName") {
                                row.cross = text_of(v);
                            } else if (field == "settleAssetId") {
                                row.settle = text_of(v);
                            } else if (field == "l2PairId") {
                                row.pair = static_cast<std::uint32_t>(u64_of(text_of(v)));
                            } else if (field == "token") {
                                token = text_of(v);
                            } else if (field == "decimals") {
                                places = u64_of(text_of(v));
                            }
                        });
                    if (!fields.has_value()) {
                        return std::unexpected(fields.error());
                    }
                    if (contracts && !row.symbol.empty() && !row.cross.empty()) {
                        rows.push_back(row);
                    } else if (!contracts && !token.empty() && places <= k_max_decimals) {
                        decimals[token] = static_cast<std::uint8_t>(places);
                    }
                }
                return {};
            });
        if (!read.has_value()) {
            return std::unexpected(read.error());
        }
        SigningMeta out;
        for (const auto& row : rows) {
            const auto places = decimals.find(row.settle);
            if (places == decimals.end()) {
                continue;
            }
            out.contracts.emplace(std::string{row.symbol},
                                  SigningContract{.cross_symbol = std::string{row.cross},
                                                  .l2_pair_id = row.pair,
                                                  .decimals = places->second});
        }
        return out;
    });
}

std::expected<L2Order, BuildError>
l2_order(const OrderTerms& terms, const SigningContract& contract, const AccountTerms& account) {
    if (account.account_id == 0) {
        return std::unexpected(BuildError::NoAccount);
    }
    const auto price = scaled(terms.price, contract.decimals);
    const auto size = scaled(terms.size, contract.decimals);
    if (!price.has_value() || !size.has_value() || *price >> 120 != 0) {
        return std::unexpected(BuildError::BadAmount);
    }
    const auto taker = fee_bps(account.taker_fee_rate);
    const auto maker = fee_bps(account.maker_fee_rate);
    if (!taker.has_value() || !maker.has_value()) {
        return std::unexpected(BuildError::BadFeeRate);
    }
    // e9 * e9 * e9 stays below 2^128 for any order this venue lists
    const auto notional = static_cast<uint128_t>(terms.size.raw) * static_cast<uint128_t>(terms.price.raw);
    const auto rate = static_cast<uint128_t>(account.taker_fee_rate.raw);
    if (rate != 0 && notional > (~uint128_t{0}) / rate) {
        return std::unexpected(BuildError::BadAmount);
    }
    const uint128_t e21 = pow10(21);
    const auto fee = (notional * rate + e21 - 1) / e21;

    const auto slot = zk::slot_nonce(terms.client_id);
    L2Order out{};
    out.contract = zk::ContractOrder{.account_id = static_cast<std::uint32_t>(account.account_id % k_max_u32),
                                     .sub_account_id = 0,
                                     .slot_id = slot.slot,
                                     .nonce = slot.nonce,
                                     .pair_id = contract.l2_pair_id,
                                     .size = *size,
                                     .price = *price,
                                     .buy = terms.side == Side::Buy,
                                     .taker_fee_rate = *taker,
                                     .maker_fee_rate = *maker,
                                     .has_subsidy = false};
    out.expiration_ms = (terms.now_s + k_expiry_s) * 1000ULL;
    out.limit_fee_e6 = static_cast<std::uint64_t>(fee);
    return out;
}

bool RequestBuilder::load(std::string_view api_key,
                          std::string_view passphrase,
                          std::string_view api_secret,
                          std::string_view zk_seeds) noexcept {
    loaded_ = false;
    if (api_key.empty() || api_secret.empty() || !printable(api_key) || !signer_.load_hex(zk_seeds)) {
        return false;
    }
    api_key_ = std::string{api_key};
    passphrase_ = std::string{passphrase};
    auto encoded = base64(api_secret);
    const auto stored = hmac_key_.assign(encoded);
    std::fill(encoded.begin(), encoded.end(), '\0');
    loaded_ = stored.has_value();
    return loaded_;
}

void RequestBuilder::set_meta(SigningMeta meta) {
    meta_ = std::move(meta);
}

void RequestBuilder::set_account(const AccountTerms& account) noexcept {
    account_ = account;
}

bool RequestBuilder::sign(std::string_view method, std::string_view path, std::string_view data, std::uint64_t now_ms) {
    timestamp_len_ = digits(now_ms, timestamp_).size();
    message_.clear();
    message_.append(timestamp_.data(), timestamp_len_);
    message_.append(method);
    message_.append(path);
    message_.append(data);
    hmac_len_ = net::hmac_sha256_base64(hmac_key_.view(), message_, hmac_).size();
    return hmac_len_ == net::k_base64_bytes;
}

net::RestRequest RequestBuilder::request(net::HttpMethod method, std::string_view target, std::string_view body) {
    net::RestRequest out{.method = method, .target = target, .body = body};
    out.headers = {{"APEX-SIGNATURE", {hmac_.data(), hmac_len_}},
                   {"APEX-API-KEY", api_key_},
                   {"APEX-TIMESTAMP", {timestamp_.data(), timestamp_len_}},
                   {"APEX-PASSPHRASE", passphrase_}};
    out.content_type = "application/x-www-form-urlencoded";
    return out;
}

std::expected<net::RestRequest, BuildError> RequestBuilder::create_order(const OrderTerms& terms,
                                                                         std::uint64_t now_ms) {
    if (!loaded_) {
        return std::unexpected(BuildError::NotSigned);
    }
    if (!printable(terms.client_id) || !printable(terms.symbol) || !printable(terms.time_in_force)) {
        return std::unexpected(BuildError::BadText);
    }
    const auto contract = meta_.contracts.find(std::string{terms.symbol});
    if (contract == meta_.contracts.end()) {
        return std::unexpected(BuildError::UnknownSymbol);
    }
    const auto order = l2_order(terms, contract->second, account_);
    if (!order.has_value()) {
        return std::unexpected(order.error());
    }
    const auto bytes = zk::contract_bytes(order->contract);
    const auto signature = signer_.sign(bytes);
    signature_[0] = '0';
    signature_[1] = 'x';
    hex_into(signature, std::span{signature_}.subspan(2));

    std::array<char, 40> price_text{};
    std::array<char, 40> size_text{};
    std::array<char, 40> fee_buf{};
    std::array<char, 24> expiry_buf{};
    const auto price = format_e9(terms.price, price_text.data(), price_text.size());
    const auto size = format_e9(terms.size, size_text.data(), size_text.size());
    const auto fee = fee_text(order->limit_fee_e6, fee_buf);
    const auto expiration = digits(order->expiration_ms, expiry_buf);
    if (price.empty() || size.empty() || fee.empty() || expiration.empty()) {
        return std::unexpected(BuildError::BadAmount);
    }
    const std::string_view no = "False";
    std::array<Param, 17> fields{{{"symbol", terms.symbol},
                                  {"side", terms.side == Side::Buy ? "BUY" : "SELL"},
                                  {"type", "LIMIT"},
                                  {"timeInForce", terms.time_in_force},
                                  {"size", size},
                                  {"price", price},
                                  {"limitFee", fee},
                                  {"expiration", expiration},
                                  {"clientId", terms.client_id},
                                  {"signature", last_signature()},
                                  {"reduceOnly", terms.reduce_only ? "True" : no},
                                  {"isPositionTpsl", no},
                                  {"isOpenTpslOrder", no},
                                  {"isSetOpenSl", no},
                                  {"isSetOpenTp", no},
                                  {"slLimitFee", "None"},
                                  {"tpLimitFee", "None"}}};
    body_.clear();
    for (const auto& [key, value] : fields) {
        body_.append(body_.empty() ? "" : "&").append(key).append("=");
        append_encoded(body_, value);
    }
    std::ranges::sort(fields, {}, &Param::first);
    std::string data;
    for (const auto& [key, value] : fields) {
        data.append(data.empty() ? "" : "&").append(key).append("=").append(value);
    }
    if (!sign("POST", k_create_order, data, now_ms)) {
        return std::unexpected(BuildError::NotSigned);
    }
    return request(net::HttpMethod::Post, k_create_order, body_);
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::cancel(std::string_view path, std::string_view id, std::uint64_t now_ms) {
    if (!loaded_) {
        return std::unexpected(BuildError::NotSigned);
    }
    if (!printable(id)) {
        return std::unexpected(BuildError::BadText);
    }
    std::string data{"id="};
    data.append(id);
    body_.assign("id=");
    append_encoded(body_, id);
    if (!sign("POST", path, data, now_ms)) {
        return std::unexpected(BuildError::NotSigned);
    }
    return request(net::HttpMethod::Post, path, body_);
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::get(std::string_view path, std::span<const Param> params, std::uint64_t now_ms) {
    if (!loaded_) {
        return std::unexpected(BuildError::NotSigned);
    }
    target_.assign(path);
    for (const auto& [key, value] : params) {
        if (!printable(value)) {
            return std::unexpected(BuildError::BadText);
        }
        target_.append(target_.size() == path.size() ? "?" : "&").append(key).append("=").append(value);
    }
    if (!sign("GET", target_, {}, now_ms)) {
        return std::unexpected(BuildError::NotSigned);
    }
    body_.clear();
    return request(net::HttpMethod::Get, target_, {});
}

std::string RequestBuilder::stream_target(std::uint64_t now_ms) {
    timestamp_len_ = digits(now_ms, timestamp_).size();
    std::string out{k_ws_private};
    out.append("?v=2&timestamp=").append(timestamp_.data(), timestamp_len_);
    return out;
}

std::string RequestBuilder::login_frame() {
    if (!loaded_) {
        return {};
    }
    const std::string ts{timestamp_.data(), timestamp_len_};
    if (!sign("GET", "/ws/accounts", {}, u64_of(ts))) {
        return {};
    }
    // the login request travels as a json string inside args
    std::string out{R"({"op":"login","args":["{\"type\":\"login\",\"topics\":[\")"};
    out.append(k_ws_topic)
        .append(R"(\"],\"httpMethod\":\"GET\",\"requestPath\":\"/ws/accounts\",\"apiKey\":\")")
        .append(api_key_)
        .append(R"(\",\"passphrase\":\")")
        .append(passphrase_)
        .append(R"(\",\"timestamp\":)")
        .append(ts)
        .append(R"(,\"signature\":\")")
        .append(hmac_.data(), hmac_len_)
        .append(R"(\"}"]})");
    return out;
}

std::expected<WireReply, WireError> parse_reply(std::string_view body) {
    return guarded([&]() -> std::expected<WireReply, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireReply out{};
        bool has_data = false;
        const auto read = json::for_each_field(
            *root, [&](std::string_view key, json::ondemand::value& value) -> std::expected<void, WireError> {
                if (key == "code") {
                    out.code = text_of(value);
                } else if (key == "msg") {
                    out.msg = text_of(value);
                } else if (key == "data") {
                    json::ondemand::json_type type{};
                    if (value.type().get(type) != simdjson::SUCCESS || type == json::ondemand::json_type::null) {
                        return {};
                    }
                    has_data = true;
                    json::ondemand::object data;
                    if (type != json::ondemand::json_type::object ||
                        value.get_object().get(data) != simdjson::SUCCESS) {
                        return {};
                    }
                    return json::for_each_field(data, [&](std::string_view field, json::ondemand::value& v) {
                        if (field == "id") {
                            out.order_id = text_of(v);
                        }
                    });
                }
                return {};
            });
        if (!read.has_value()) {
            return std::unexpected(read.error());
        }
        out.ok = has_data && (out.code.empty() || out.code == "0");
        if (!out.ok && out.code.empty()) {
            return std::unexpected(WireError::MissingField);
        }
        return out;
    });
}

ErrorClass classify(int http_status, std::string_view code) noexcept {
    if (http_status == 429 || http_status == 403) {
        return ErrorClass::RateLimited;
    }
    // 10002 refuses the key, 20002 the timestamp
    if (http_status == 401 || code == "10002" || code == "20002") {
        return ErrorClass::AuthFailure;
    }
    if (http_status >= 500) {
        return ErrorClass::Retryable;
    }
    return ErrorClass::Terminal;
}

bool is_benign_cancel_failure(std::string_view msg) noexcept {
    return msg.find("not exist") != std::string_view::npos || msg.find("not found") != std::string_view::npos ||
           msg.find("already") != std::string_view::npos;
}

std::expected<AccountRows, WireError> parse_account(std::string_view body, PrivateBuffer& storage) {
    return guarded([&]() -> std::expected<AccountRows, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = json::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        AccountRows out{};
        std::string_view taker;
        std::string_view maker;
        TradeRows rows{};
        const auto read = json::for_each_field(
            *data, [&](std::string_view key, json::ondemand::value& value) -> std::expected<void, WireError> {
                if (key == "id") {
                    out.terms.account_id = u64_of(text_of(value));
                    return {};
                }
                if (key == "positions") {
                    return read_group(key, value, storage, rows, false);
                }
                json::ondemand::object contract;
                if (key != "contractAccount" || value.get_object().get(contract) != simdjson::SUCCESS) {
                    return {};
                }
                return json::for_each_field(contract, [&](std::string_view field, json::ondemand::value& v) {
                    if (field == "takerFeeRate") {
                        taker = text_of(v);
                    } else if (field == "makerFeeRate") {
                        maker = text_of(v);
                    }
                });
            });
        if (!read.has_value()) {
            return std::unexpected(read.error());
        }
        const auto taker_rate = decimal_e9(taker);
        const auto maker_rate = decimal_e9(maker);
        if (out.terms.account_id == 0 || !taker_rate.has_value() || !maker_rate.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.terms.taker_fee_rate = *taker_rate;
        out.terms.maker_fee_rate = *maker_rate;
        out.positions = rows.positions;
        return out;
    });
}

std::expected<TradeRows, WireError> parse_rest_rows(std::string_view body, PrivateBuffer& storage, bool fills) {
    return guarded([&]() -> std::expected<TradeRows, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = data_of(*root);
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        json::ondemand::json_type type{};
        if (const auto ec = data->type().get(type); ec != simdjson::SUCCESS) {
            return std::unexpected(json::map_error(ec));
        }
        TradeRows out{};
        if (type == json::ondemand::json_type::array) {
            const auto count = fills ? read_rows(*data, storage.fills, fill_fill)
                                     : read_rows(*data, storage.orders, fill_order);
            if (!count.has_value()) {
                return std::unexpected(count.error());
            }
            if (fills) {
                out.fills = {storage.fills.data(), *count};
            } else {
                out.orders = {storage.orders.data(), *count};
            }
            return out;
        }
        json::ondemand::object page;
        if (const auto ec = data->get_object().get(page); ec != simdjson::SUCCESS) {
            return std::unexpected(json::map_error(ec));
        }
        WireOrder single{};
        bool paged = false;
        const auto read = json::for_each_field(
            page, [&](std::string_view key, json::ondemand::value& value) -> std::expected<void, WireError> {
                if (key == "orders" || key == "fills") {
                    paged = true;
                    return read_group(key, value, storage, out, fills);
                }
                fill_order(single, key, value);
                return {};
            });
        if (!read.has_value()) {
            return std::unexpected(read.error());
        }
        if (!paged && !fills && !single.id.empty()) {
            storage.orders[0] = single;
            out.orders = {storage.orders.data(), 1};
        }
        return out;
    });
}

std::expected<PrivateFrame, WireError> parse_private_kind(std::string_view frame) {
    return guarded([&]() -> std::expected<PrivateFrame, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        PrivateFrame out{};
        const auto read = json::for_each_field(
            *root, [&](std::string_view key, json::ondemand::value& value) -> std::expected<void, WireError> {
                if (key == "op") {
                    if (text_of(value) == "ping") {
                        out.kind = PrivateKind::Ping;
                    }
                } else if (key == "args" && out.kind == PrivateKind::Ping) {
                    json::ondemand::array args;
                    if (value.get_array().get(args) != simdjson::SUCCESS) {
                        return {};
                    }
                    for (auto arg : args) {
                        json::ondemand::value v;
                        if (arg.get(v) == simdjson::SUCCESS) {
                            out.value = text_of(v);
                        }
                        break;
                    }
                } else if (key == "success") {
                    out.kind = PrivateKind::Ack;
                    out.success = text_of(value) == "true";
                } else if (key == "request" && out.kind == PrivateKind::Ack) {
                    json::ondemand::object request;
                    if (value.get_object().get(request) == simdjson::SUCCESS) {
                        const auto op = json::opt_str(request, "op");
                        out.value = op.has_value() ? *op : std::string_view{};
                    }
                } else if (key == "topic") {
                    if (text_of(value) == k_ws_topic) {
                        out.kind = PrivateKind::Account;
                    }
                } else if (key == "type") {
                    out.snapshot = text_of(value) == "snapshot";
                }
                return {};
            });
        if (!read.has_value()) {
            return std::unexpected(read.error());
        }
        return out;
    });
}

std::expected<TradeRows, WireError> parse_account_push(std::string_view frame, PrivateBuffer& storage) {
    return guarded([&]() -> std::expected<TradeRows, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto contents = json::req_object(*root, "contents");
        if (!contents.has_value()) {
            return std::unexpected(contents.error());
        }
        TradeRows out{};
        const auto read = json::for_each_field(
            *contents, [&](std::string_view key, json::ondemand::value& value) -> std::expected<void, WireError> {
                if (key == "orders" || key == "fills" || key == "positions") {
                    return read_group(key, value, storage, out, false);
                }
                return {};
            });
        if (!read.has_value()) {
            return std::unexpected(read.error());
        }
        return out;
    });
}

std::optional<OrderEvent> map_order(const WireOrder& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto side = side_of(wire.side);
    const auto size = lots_of(wire.size, base);
    const auto filled = lots_of(wire.cum_fill_size, base);
    if (!side.has_value() || !size.has_value() || !filled.has_value()) {
        return std::nullopt;
    }
    OrderEvent out{};
    if ((wire.status == "OPEN" || wire.status == "PENDING") && filled->raw == 0) {
        out.type = OrderEventType::Accepted;
        out.leaves_qty = *size;
    } else if (wire.status == "CANCELED" || wire.status == "EXPIRED") {
        out.type = OrderEventType::Canceled;
        out.leaves_qty = QtyLots::from_raw(0);
    } else {
        return std::nullopt;
    }
    out.instrument = base.id;
    out.side = *side;
    out.qty = *size;
    if (const auto price = ticks_of(wire.price, base); price.has_value()) {
        out.price = *price;
    }
    if (const auto id = ExchangeClientOrderId::from(wire.client_order_id); id.has_value()) {
        out.exchange_client_order_id = *id;
    }
    if (const auto id = ExchangeOrderId::from(wire.id); id.has_value()) {
        out.exchange_order_id = *id;
    }
    out.ts_event = from_ms(wire.updated_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<exec::OrderStatusReport> map_order_report(const WireOrder& wire, const InstrumentBase& base) {
    const auto side = side_of(wire.side);
    const auto size = lots_of(wire.size, base);
    const auto filled = lots_of(wire.cum_fill_size, base);
    const auto client = ExchangeClientOrderId::from(wire.client_order_id);
    const auto id = ExchangeOrderId::from(wire.id);
    if (!side.has_value() || !size.has_value() || !filled.has_value() || !client.has_value() || !id.has_value()) {
        return std::nullopt;
    }
    exec::OrderStatusReport out{};
    out.exchange_client_order_id = *client;
    out.exchange_order_id = *id;
    out.instrument = base.id;
    out.side = *side;
    out.qty = *size;
    out.leaves = QtyLots::from_raw(size->raw > filled->raw ? size->raw - filled->raw : 0);
    if (wire.status == "FILLED") {
        out.status = OrderStatus::Filled;
        out.leaves = QtyLots::from_raw(0);
    } else if (wire.status == "CANCELED" || wire.status == "EXPIRED") {
        out.status = OrderStatus::Canceled;
        out.leaves = QtyLots::from_raw(0);
    } else if (filled->raw > 0) {
        out.status = OrderStatus::PartiallyFilled;
    } else {
        out.status = OrderStatus::Accepted;
    }
    if (const auto price = ticks_of(wire.price, base); price.has_value()) {
        out.price = *price;
    }
    out.ts_venue = from_ms(wire.updated_ms, UnixNanos{});
    return out;
}

std::optional<OrderEvent>
map_fill(const WireFill& wire, const InstrumentBase& base, QtyLots leaves, UnixNanos ts_init) {
    const auto side = side_of(wire.side);
    const auto price = ticks_of(wire.price, base);
    const auto qty = lots_of(wire.size, base);
    const auto exec_id = ExecId::from(wire.id);
    if (!side.has_value() || !price.has_value() || !qty.has_value() || qty->raw <= 0 || !exec_id.has_value() ||
        wire.id.empty()) {
        return std::nullopt;
    }
    OrderEvent out{};
    out.type = OrderEventType::Filled;
    out.instrument = base.id;
    out.side = *side;
    out.liquidity = wire.liquidity == "MAKER"   ? Liquidity::Maker
                    : wire.liquidity == "TAKER" ? Liquidity::Taker
                                                : Liquidity::Unknown;
    out.exec_id = *exec_id;
    if (const auto id = ExchangeClientOrderId::from(wire.client_order_id); id.has_value()) {
        out.exchange_client_order_id = *id;
    }
    if (const auto id = ExchangeOrderId::from(wire.order_id); id.has_value()) {
        out.exchange_order_id = *id;
    }
    out.price = *price;
    out.qty = *qty;
    out.leaves_qty = leaves;
    if (const auto fee = decimal_e9(wire.fee); fee.has_value()) {
        out.fee = *fee;
    }
    out.ts_event = from_ms(wire.created_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<exec::PositionReport> map_position(const WirePosition& wire, const InstrumentBase& base) {
    const auto lots = lots_of(wire.size, base);
    if (!lots.has_value() || (wire.side != "LONG" && wire.side != "SHORT")) {
        return std::nullopt;
    }
    exec::PositionReport out{};
    out.instrument = base.id;
    out.net_qty = wire.side == "SHORT" ? QtyLots::from_raw(-lots->raw) : *lots;
    if (const auto entry = ticks_of(wire.entry_price, base); entry.has_value()) {
        out.avg_entry = *entry;
    }
    return out;
}

} // namespace raikiri::venues::apex
