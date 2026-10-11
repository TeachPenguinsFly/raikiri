#include <raikiri/common/fixed_point.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/venues/edgex/trade.hpp>
#include <raikiri/wire/json.hpp>

#include <charconv>
#include <cstring>

namespace raikiri::venues::edgex {

namespace json = raikiri::wire;

namespace {

using json::guarded;
using json::session;

constexpr std::uint64_t k_day_ms = 86'400'000ULL;
constexpr std::uint64_t k_l2_window_ms = 30 * k_day_ms;
constexpr std::uint64_t k_l1_offset_ms = 8 * k_day_ms;
constexpr std::uint8_t k_max_scale = 36;

constexpr std::string_view k_domain_type =
    "EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)";
constexpr std::string_view k_base_type =
    "OrderBase(uint256 nonce,address signer,uint64 accountId,uint256 expirationTimestamp)";
constexpr std::string_view k_limit_type =
    "LimitOrderParams(OrderBase base,int256 amountSynthetic,int256 amountCollateral,uint256 amountFee,"
    "uint64 assetIdSynthetic,uint64 assetIdCollateral,bool isBuyingSynthetic,bytes32 parentOrderHash,"
    "uint256 subOrderIndex)"
    "OrderBase(uint256 nonce,address signer,uint64 accountId,uint256 expirationTimestamp)";

[[nodiscard]] unsigned __int128 pow10(std::uint8_t exponent) noexcept {
    unsigned __int128 out = 1;
    for (std::uint8_t i = 0; i < exponent; ++i) {
        out *= 10;
    }
    return out;
}

[[nodiscard]] evm::Hash hash_text(std::string_view text) noexcept {
    return evm::keccak256({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}); // NOLINT
}

class Words {
public:
    void hash(const evm::Hash& value) noexcept { put(value); }

    void uint(unsigned __int128 value) noexcept {
        evm::Hash word{};
        for (std::size_t i = 0; i < 16; ++i) {
            word[31 - i] = static_cast<std::uint8_t>(value >> (8U * i));
        }
        put(word);
    }

    void address(std::span<const std::uint8_t, 20> value) noexcept {
        evm::Hash word{};
        std::memcpy(word.data() + 12, value.data(), value.size());
        put(word);
    }

    [[nodiscard]] evm::Hash digest() const noexcept { return evm::keccak256({bytes_.data(), used_}); }

private:
    void put(const evm::Hash& word) noexcept {
        if (used_ + word.size() <= bytes_.size()) {
            std::memcpy(bytes_.data() + used_, word.data(), word.size());
            used_ += word.size();
        }
    }

    std::array<std::uint8_t, 32 * 10> bytes_{};
    std::size_t used_{0};
};

[[nodiscard]] std::string_view digits(std::uint64_t value, std::span<char> out) noexcept {
    const auto [stop, ec] = std::to_chars(out.data(), out.data() + out.size(), value);
    return ec == std::errc{} ? std::string_view{out.data(), static_cast<std::size_t>(stop - out.data())}
                             : std::string_view{};
}

[[nodiscard]] std::string_view digits128(unsigned __int128 value, std::span<char> out) noexcept {
    std::array<char, 40> reversed{};
    std::size_t count = 0;
    do {
        reversed[count++] = static_cast<char>('0' + static_cast<int>(value % 10));
        value /= 10;
    } while (value != 0 && count < reversed.size());
    if (count > out.size()) {
        return {};
    }
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = reversed[count - 1 - i];
    }
    return {out.data(), count};
}

// standard alphabet with padding
[[nodiscard]] std::string base64(std::string_view in) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < in.size(); i += 3) {
        const auto rest = in.size() - i;
        const std::uint32_t chunk = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[i])) << 16U) |
                                    (rest > 1 ? static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[i + 1])) << 8U : 0U) |
                                    (rest > 2 ? static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[i + 2])) : 0U);
        out.push_back(alphabet[(chunk >> 18U) & 0x3FU]);
        out.push_back(alphabet[(chunk >> 12U) & 0x3FU]);
        out.push_back(rest > 1 ? alphabet[(chunk >> 6U) & 0x3FU] : '=');
        out.push_back(rest > 2 ? alphabet[chunk & 0x3FU] : '=');
    }
    return out;
}

[[nodiscard]] bool json_safe(std::string_view text) noexcept {
    for (const auto c : text) {
        if (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::uint32_t id_of(std::string_view text) noexcept {
    std::uint32_t out = 0;
    const auto* end = text.data() + text.size();
    const auto [stop, ec] = std::from_chars(text.data(), end, out);
    return ec == std::errc{} && stop == end ? out : 0;
}

[[nodiscard]] std::uint64_t u64_of(std::string_view text) noexcept {
    std::uint64_t out = 0;
    const auto* end = text.data() + text.size();
    const auto [stop, ec] = std::from_chars(text.data(), end, out);
    return ec == std::errc{} && stop == end ? out : 0;
}

// strings as their contents, numbers and booleans as their raw text
[[nodiscard]] std::string_view text_of(json::ondemand::value& value) {
    if (const auto text = json::str_of(value); text.has_value()) {
        return *text;
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
        const auto read = json::for_each_field(obj, [&](std::string_view key, json::ondemand::value& field) {
            fill(row, key, field);
        });
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
    } else if (key == "clientOrderId") {
        row.client_order_id = text_of(value);
    } else if (key == "contractId") {
        row.contract_id = id_of(text_of(value));
    } else if (key == "side") {
        row.side = text_of(value);
    } else if (key == "price") {
        row.price = text_of(value);
    } else if (key == "size") {
        row.size = text_of(value);
    } else if (key == "status") {
        row.status = text_of(value);
    } else if (key == "cumMatchSize") {
        row.cum_match_size = text_of(value);
    } else if (key == "cumFillSize" && row.cum_match_size.empty()) {
        // the rest rows carry only the settled figure
        row.cum_match_size = text_of(value);
    } else if (key == "cancelReason") {
        row.cancel_reason = text_of(value);
    } else if (key == "updatedTime") {
        row.updated_ms = u64_of(text_of(value));
    }
}

void fill_fill(WireFill& row, std::string_view key, json::ondemand::value& value) {
    if (key == "id") {
        row.id = text_of(value);
    } else if (key == "orderId") {
        row.order_id = text_of(value);
    } else if (key == "clientOrderId" || key == "orderClientOrderId") {
        row.client_order_id = text_of(value);
    } else if (key == "contractId") {
        row.contract_id = id_of(text_of(value));
    } else if (key == "orderSide" || key == "side") {
        row.side = text_of(value);
    } else if (key == "fillSize") {
        row.size = text_of(value);
    } else if (key == "fillPrice") {
        row.price = text_of(value);
    } else if (key == "fillFee") {
        row.fee = text_of(value);
    } else if (key == "direction") {
        row.direction = text_of(value);
    } else if (key == "matchTime") {
        row.match_ms = u64_of(text_of(value));
    }
}

void fill_position(WirePosition& row, std::string_view key, json::ondemand::value& value) {
    if (key == "contractId") {
        row.contract_id = id_of(text_of(value));
    } else if (key == "openSize") {
        row.open_size = text_of(value);
    } else if (key == "openValue") {
        row.open_value = text_of(value);
    }
}

void fill_collateral(WireCollateral& row, std::string_view key, json::ondemand::value& value) {
    if (key == "coinId") {
        row.coin_id = id_of(text_of(value));
    } else if (key == "amount") {
        row.amount = text_of(value);
    }
}

// one group per key; groups this adapter does not read are skipped
std::expected<TradeRows, WireError> read_groups(json::ondemand::object& data, TradeBuffer& storage) {
    TradeRows out{};
    const auto read = json::for_each_field(data, [&](std::string_view key, json::ondemand::value& value)
                                                     -> std::expected<void, WireError> {
        std::expected<std::size_t, WireError> count{0};
        if (key == "order" || key == "dataList") {
            count = read_rows(value, storage.orders, fill_order);
            if (count.has_value()) {
                out.orders = {storage.orders.data(), *count};
            }
        } else if (key == "orderFillTransaction") {
            count = read_rows(value, storage.fills, fill_fill);
            if (count.has_value()) {
                out.fills = {storage.fills.data(), *count};
            }
        } else if (key == "position" || key == "positionList") {
            count = read_rows(value, storage.positions, fill_position);
            if (count.has_value()) {
                out.positions = {storage.positions.data(), *count};
            }
        } else if (key == "collateral" || key == "collateralList") {
            count = read_rows(value, storage.collateral, fill_collateral);
            if (count.has_value()) {
                out.collateral = {storage.collateral.data(), *count};
            }
        }
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        return {};
    });
    if (!read.has_value()) {
        return std::unexpected(read.error());
    }
    return out;
}

std::expected<json::ondemand::value, WireError> success_data(json::ondemand::object& root) {
    const auto code = json::req_str(root, "code");
    if (!code.has_value() || *code != "SUCCESS") {
        return std::unexpected(WireError::UnknownMessage);
    }
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

[[nodiscard]] std::optional<PriceTicks> ticks_of(std::string_view text, const InstrumentBase& base) noexcept {
    const auto value = parse_decimal_e9(text);
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
    const auto value = parse_decimal_e9(text);
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

std::expected<Decimal, WireError> parse_decimal(std::string_view text) noexcept {
    Decimal out{};
    bool point = false;
    std::size_t count = 0;
    for (const auto c : text) {
        if (c == '.' && !point) {
            point = true;
            continue;
        }
        if (c < '0' || c > '9' || count == k_max_scale) {
            return std::unexpected(WireError::WrongType);
        }
        out.mantissa = out.mantissa * 10 + static_cast<unsigned>(c - '0');
        ++count;
        if (point) {
            ++out.scale;
        }
    }
    if (count == 0) {
        return std::unexpected(WireError::WrongType);
    }
    return out;
}

std::string_view format_decimal(Decimal value, std::span<char> out) noexcept {
    std::array<char, 40> raw{};
    const auto body = digits128(value.mantissa, raw);
    // python switches to exponent notation below 1e-6
    if (body.empty() || static_cast<int>(body.size()) - 1 - value.scale < -6) {
        return {};
    }
    const std::size_t integer = body.size() > value.scale ? body.size() - value.scale : 1;
    const std::size_t total = integer + (value.scale != 0 ? 1U + value.scale : 0U);
    if (total > out.size()) {
        return {};
    }
    std::size_t at = 0;
    const std::size_t padded = integer + value.scale;
    for (std::size_t i = 0; i < padded; ++i) {
        if (i == integer) {
            out[at++] = '.';
        }
        const std::size_t lead = padded - body.size();
        out[at++] = i < lead ? '0' : body[i - lead];
    }
    return {out.data(), at};
}

std::expected<SigningMeta, WireError> parse_signing_meta(std::string_view body) {
    return guarded([&]() -> std::expected<SigningMeta, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto value = success_data(*root);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        json::ondemand::object data;
        if (const auto ec = value->get_object().get(data); ec != simdjson::SUCCESS) {
            return std::unexpected(json::map_error(ec));
        }
        SigningMeta meta;
        std::unordered_map<std::uint32_t, std::uint64_t> coin_resolution;
        struct Row {
            std::uint32_t id{0};
            std::uint64_t resolution{0};
            std::uint32_t quote{0};
            std::string_view taker;
            std::string_view maker;
        };
        std::vector<Row> rows;
        std::string_view native_chain;
        std::string_view chain;
        std::string_view contract;
        const auto read = json::for_each_field(data, [&](std::string_view key, json::ondemand::value& group)
                                                         -> std::expected<void, WireError> {
            if (key == "global") {
                json::ondemand::object global;
                if (const auto ec = group.get_object().get(global); ec != simdjson::SUCCESS) {
                    return std::unexpected(json::map_error(ec));
                }
                return json::for_each_field(global, [&](std::string_view field, json::ondemand::value& v) {
                    if (field == "nativeChainId") {
                        native_chain = text_of(v);
                    } else if (field == "chainId") {
                        chain = text_of(v);
                    } else if (field == "contractAddress") {
                        contract = text_of(v);
                    }
                });
            }
            json::ondemand::array list;
            if (key != "coinList" && key != "contractList") {
                return {};
            }
            if (const auto ec = group.get_array().get(list); ec != simdjson::SUCCESS) {
                return std::unexpected(json::map_error(ec));
            }
            for (auto entry : list) {
                json::ondemand::object obj;
                if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                    return std::unexpected(json::map_error(ec));
                }
                Row row{};
                std::string_view resolution;
                const auto fields = json::for_each_field(obj, [&](std::string_view field, json::ondemand::value& v) {
                    if (field == "coinId" || field == "contractId") {
                        row.id = id_of(text_of(v));
                    } else if (field == "resolution") {
                        resolution = text_of(v);
                    } else if (field == "quoteCoinId") {
                        row.quote = id_of(text_of(v));
                    } else if (field == "defaultTakerFeeRate") {
                        row.taker = text_of(v);
                    } else if (field == "defaultMakerFeeRate") {
                        row.maker = text_of(v);
                    }
                });
                if (!fields.has_value()) {
                    return std::unexpected(fields.error());
                }
                // coins hold hex ("0xf4240"), contracts decimal ("10000000000")
                if (resolution.starts_with("0x")) {
                    const auto hex = resolution.substr(2);
                    const auto [stop, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), row.resolution, 16);
                    if (ec != std::errc{} || stop != hex.data() + hex.size()) {
                        row.resolution = 0;
                    }
                } else {
                    row.resolution = u64_of(resolution);
                }
                if (key == "coinList") {
                    if (row.id != 0 && row.resolution != 0) {
                        coin_resolution.emplace(row.id, row.resolution);
                    }
                } else if (row.id != 0 && row.resolution != 0) {
                    rows.push_back(row);
                }
            }
            return {};
        });
        if (!read.has_value()) {
            return std::unexpected(read.error());
        }
        const auto chain_text = native_chain.empty() ? chain : native_chain;
        if (chain_text.starts_with("0x")) {
            const auto hex = chain_text.substr(2);
            (void)std::from_chars(hex.data(), hex.data() + hex.size(), meta.chain_id, 16);
        } else {
            meta.chain_id = u64_of(chain_text);
        }
        if (meta.chain_id == 0 || !evm::from_hex(contract, meta.verifying_contract)) {
            return std::unexpected(WireError::MissingField);
        }
        const Decimal fallback{.mantissa = 1, .scale = 3};
        for (const auto& row : rows) {
            const auto quote = coin_resolution.find(row.quote);
            if (quote == coin_resolution.end()) {
                continue; // unsignable without the collateral's resolution
            }
            SigningContract out{.contract_id = row.id,
                                .resolution = row.resolution,
                                .collateral_id = row.quote,
                                .collateral_resolution = quote->second};
            const auto taker = row.taker.empty() ? fallback : parse_decimal(row.taker).value_or(fallback);
            const auto maker = row.maker.empty() ? fallback : parse_decimal(row.maker).value_or(fallback);
            // compared at a common scale
            const auto scale = std::max(taker.scale, maker.scale);
            const auto t = taker.mantissa * pow10(static_cast<std::uint8_t>(scale - taker.scale));
            const auto m = maker.mantissa * pow10(static_cast<std::uint8_t>(scale - maker.scale));
            out.fee_rate = t >= m ? taker : maker;
            meta.contracts.emplace(row.id, out);
        }
        return meta;
    });
}

std::uint32_t l2_nonce(std::string_view client_order_id) noexcept {
    std::array<char, 64> hex{};
    const auto digest = net::sha256_hex(client_order_id, hex);
    std::uint32_t out = 0;
    if (digest.size() >= 8) {
        (void)std::from_chars(digest.data(), digest.data() + 8, out, 16);
    }
    return out;
}

std::string_view to_string(BuildError err) noexcept {
    switch (err) {
    case BuildError::NotSigned:
        return "edgex: no trading key or api credentials loaded";
    case BuildError::UnknownContract:
        return "edgex: no signing metadata for the contract";
    case BuildError::BadDecimal:
        return "edgex: price or size is not a plain decimal";
    case BuildError::BufferTooSmall:
        return "edgex: request did not fit";
    }
    return "edgex: unknown build error";
}

std::expected<L2Order, BuildError> l2_order(const OrderTerms& terms, const SigningContract& contract) {
    const auto price = parse_decimal(terms.price);
    const auto size = parse_decimal(terms.size);
    if (!price.has_value() || !size.has_value() || price->scale + size->scale > k_max_scale) {
        return std::unexpected(BuildError::BadDecimal);
    }
    L2Order out{};
    out.nonce = l2_nonce(terms.client_order_id);
    out.l2_expire_ms = terms.expire_ms != 0 ? terms.expire_ms + k_l1_offset_ms : terms.now_ms + k_l2_window_ms;
    out.expire_ms = out.l2_expire_ms - k_l1_offset_ms;
    out.value = Decimal{.mantissa = price->mantissa * size->mantissa,
                        .scale = static_cast<std::uint8_t>(price->scale + size->scale)};
    const auto fee_scale = static_cast<std::uint8_t>(out.value.scale + contract.fee_rate.scale);
    if (fee_scale > 38) {
        return std::unexpected(BuildError::BadDecimal);
    }
    const auto fee_raw = out.value.mantissa * contract.fee_rate.mantissa;
    const auto unit = pow10(fee_scale);
    out.limit_fee = static_cast<std::uint64_t>((fee_raw + unit - 1) / unit);
    out.amount_synthetic = size->mantissa * contract.resolution / pow10(size->scale);
    out.amount_collateral = out.value.mantissa * contract.collateral_resolution / pow10(out.value.scale);
    out.amount_fee = static_cast<unsigned __int128>(out.limit_fee) * contract.collateral_resolution;
    return out;
}

evm::Hash domain_separator(const SigningMeta& meta) noexcept {
    Words words;
    words.hash(hash_text(k_domain_type));
    words.hash(hash_text("EdgeX"));
    words.hash(hash_text("1"));
    words.uint(meta.chain_id);
    words.address(meta.verifying_contract);
    return words.digest();
}

evm::Hash order_digest(const evm::Hash& domain,
                       const OrderTerms& terms,
                       const SigningContract& contract,
                       const L2Order& order,
                       std::span<const std::uint8_t, 20> signer) noexcept {
    static const auto base_type = hash_text(k_base_type);
    static const auto limit_type = hash_text(k_limit_type);
    Words base;
    base.hash(base_type);
    base.uint(order.nonce);
    base.address(signer);
    base.uint(terms.account_id);
    base.uint(order.expire_ms / 1000);

    Words limit;
    limit.hash(limit_type);
    limit.hash(base.digest());
    limit.uint(order.amount_synthetic);
    limit.uint(order.amount_collateral);
    limit.uint(order.amount_fee);
    limit.uint(contract.contract_id);
    limit.uint(contract.collateral_id);
    limit.uint(terms.side == Side::Buy ? 1U : 0U);
    limit.hash(evm::Hash{}); // parentOrderHash
    limit.uint(0);           // subOrderIndex
    const auto message = limit.digest();

    std::array<std::uint8_t, 66> envelope{};
    envelope[0] = 0x19;
    envelope[1] = 0x01;
    std::memcpy(envelope.data() + 2, domain.data(), domain.size());
    std::memcpy(envelope.data() + 34, message.data(), message.size());
    return evm::keccak256(envelope);
}

bool RequestBuilder::load(std::uint64_t account_id,
                          std::string_view api_key,
                          std::string_view passphrase,
                          std::string_view api_secret,
                          std::string_view trading_key) noexcept {
    loaded_ = false;
    if (account_id == 0 || api_key.empty() || api_secret.empty() || !wallet_.load(trading_key) ||
        !evm::from_hex(wallet_.address(), signer_bytes_)) {
        return false;
    }
    account_id_ = account_id;
    account_len_ = digits(account_id, account_text_).size();
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
    domain_ = domain_separator(meta_);
}

bool RequestBuilder::sign(std::string_view method, std::string_view path, std::string_view payload, std::uint64_t now_ms) {
    timestamp_len_ = digits(now_ms, timestamp_).size();
    message_.clear();
    message_.append(timestamp_.data(), timestamp_len_);
    message_.append(method);
    message_.append(path);
    message_.append(payload);
    return net::hmac_sha256_hex(hmac_key_.view(), message_, hmac_hex_).size() == hmac_hex_.size();
}

net::RestRequest RequestBuilder::request(net::HttpMethod method, std::string_view target, std::string_view body) {
    net::RestRequest out{.method = method, .target = target, .body = body};
    out.headers = {{"X-edgeX-Api-Key", api_key_},
                   {"X-edgeX-Passphrase", passphrase_},
                   {"X-edgeX-Signature", {hmac_hex_.data(), hmac_hex_.size()}},
                   {"X-edgeX-Timestamp", {timestamp_.data(), timestamp_len_}}};
    return out;
}

std::expected<net::RestRequest, BuildError> RequestBuilder::create_order(const OrderTerms& terms) {
    if (!loaded_) {
        return std::unexpected(BuildError::NotSigned);
    }
    const auto contract = meta_.contracts.find(terms.contract_id);
    if (contract == meta_.contracts.end()) {
        return std::unexpected(BuildError::UnknownContract);
    }
    if (!json_safe(terms.client_order_id) || !json_safe(terms.price) || !json_safe(terms.size)) {
        return std::unexpected(BuildError::BadDecimal);
    }
    const auto order = l2_order(terms, contract->second);
    if (!order.has_value()) {
        return std::unexpected(order.error());
    }
    evm::Signature signature{};
    if (!wallet_.sign(order_digest(domain_, terms, contract->second, *order, signer_bytes_), signature)) {
        return std::unexpected(BuildError::NotSigned);
    }
    // edgex takes r || s || v as 130 hex digits without the 0x
    std::array<char, evm::k_rsv_hex_bytes> rsv{};
    const auto hex = evm::rsv_hex(signature, rsv);
    if (hex.size() != signature_.size() + 2) {
        return std::unexpected(BuildError::NotSigned);
    }
    std::copy_n(hex.data() + 2, signature_.size(), signature_.data());

    std::array<char, 24> contract_text{};
    std::array<char, 24> expire_text{};
    std::array<char, 24> l2_expire_text{};
    std::array<char, 16> nonce_text{};
    std::array<char, 24> fee_text{};
    std::array<char, 48> value_text{};
    const std::string_view account{account_text_.data(), account_len_};
    const auto contract_id = digits(terms.contract_id, contract_text);
    const auto expire = digits(order->expire_ms, expire_text);
    const auto l2_expire = digits(order->l2_expire_ms, l2_expire_text);
    const auto nonce = digits(order->nonce, nonce_text);
    const auto fee = digits(order->limit_fee, fee_text);
    const auto value = format_decimal(order->value, value_text);
    if (value.empty()) {
        return std::unexpected(BuildError::BadDecimal);
    }
    const std::string_view sig{signature_.data(), signature_.size()};
    const std::string_view side = terms.side == Side::Buy ? "BUY" : "SELL";
    const std::string_view reduce = terms.reduce_only ? "true" : "false";

    // the sdk's insertion order
    body_.clear();
    const auto field = [this](std::string_view key, std::string_view text) {
        body_.append(body_.empty() ? "{\"" : ",\"").append(key).append("\":\"").append(text).append("\"");
    };
    const auto literal = [this](std::string_view key, std::string_view text) {
        body_.append(",\"").append(key).append("\":").append(text);
    };
    field("accountId", account);
    field("contractId", contract_id);
    field("price", terms.price);
    field("size", terms.size);
    field("type", "LIMIT");
    field("side", side);
    field("timeInForce", terms.time_in_force);
    field("clientOrderId", terms.client_order_id);
    field("expireTime", expire);
    field("l2Nonce", nonce);
    field("l2Signature", sig);
    field("l2ExpireTime", l2_expire);
    field("l2Value", value);
    field("l2Size", terms.size);
    field("l2LimitFee", fee);
    literal("reduceOnly", reduce);
    field("triggerPrice", "");
    field("triggerPriceType", "");
    field("sourceKey", "");
    literal("isPositionTpsl", "false");
    field("openTpslParentOrderId", "");
    literal("isSetOpenTp", "false");
    literal("openTp", "null");
    literal("isSetOpenSl", "false");
    literal("openSl", "null");
    field("extraType", "");
    field("extraDataJson", "");
    body_.push_back('}');

    // the same fields sorted by key, nulls as empty values
    std::string payload;
    payload.reserve(body_.size());
    const std::array<Param, 27> sorted{{{"accountId", account},
                                        {"clientOrderId", terms.client_order_id},
                                        {"contractId", contract_id},
                                        {"expireTime", expire},
                                        {"extraDataJson", ""},
                                        {"extraType", ""},
                                        {"isPositionTpsl", "false"},
                                        {"isSetOpenSl", "false"},
                                        {"isSetOpenTp", "false"},
                                        {"l2ExpireTime", l2_expire},
                                        {"l2LimitFee", fee},
                                        {"l2Nonce", nonce},
                                        {"l2Signature", sig},
                                        {"l2Size", terms.size},
                                        {"l2Value", value},
                                        {"openSl", ""},
                                        {"openTp", ""},
                                        {"openTpslParentOrderId", ""},
                                        {"price", terms.price},
                                        {"reduceOnly", reduce},
                                        {"side", side},
                                        {"size", terms.size},
                                        {"sourceKey", ""},
                                        {"timeInForce", terms.time_in_force},
                                        {"triggerPrice", ""},
                                        {"triggerPriceType", ""},
                                        {"type", "LIMIT"}}};
    for (const auto& [key, text] : sorted) {
        payload.append(payload.empty() ? "" : "&").append(key).append("=").append(text);
    }
    if (!sign("POST", k_create_order, payload, terms.now_ms)) {
        return std::unexpected(BuildError::NotSigned);
    }
    return request(net::HttpMethod::Post, k_create_order, body_);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::cancel_order(std::string_view client_order_id,
                                                                         std::uint64_t now_ms) {
    if (!loaded_) {
        return std::unexpected(BuildError::NotSigned);
    }
    if (!json_safe(client_order_id)) {
        return std::unexpected(BuildError::BadDecimal);
    }
    const std::string_view account{account_text_.data(), account_len_};
    body_.assign("{\"accountId\":\"")
        .append(account)
        .append("\",\"clientOrderIdList\":[\"")
        .append(client_order_id)
        .append("\"]}");
    std::string payload{"accountId="};
    payload.append(account).append("&clientOrderIdList=").append(client_order_id);
    if (!sign("POST", k_cancel_by_client_id, payload, now_ms)) {
        return std::unexpected(BuildError::NotSigned);
    }
    return request(net::HttpMethod::Post, k_cancel_by_client_id, body_);
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::get(std::string_view path, std::span<const Param> params, std::uint64_t now_ms) {
    if (!loaded_) {
        return std::unexpected(BuildError::NotSigned);
    }
    std::string payload;
    for (const auto& [key, text] : params) {
        payload.append(payload.empty() ? "" : "&").append(key).append("=").append(text);
    }
    if (!sign("GET", path, payload, now_ms)) {
        return std::unexpected(BuildError::NotSigned);
    }
    target_.assign(path);
    if (!payload.empty()) {
        target_.append("?").append(payload);
    }
    body_.clear();
    return request(net::HttpMethod::Get, target_, {});
}

std::vector<std::pair<std::string, std::string>> RequestBuilder::stream_headers(std::uint64_t now_ms) {
    if (!loaded_) {
        return {};
    }
    std::array<char, 24> ts{};
    std::string payload{"accountId="};
    payload.append(account_text_.data(), account_len_).append("&timestamp=").append(digits(now_ms, ts));
    if (!sign("GET", k_private_ws, payload, now_ms)) {
        return {};
    }
    return {{"X-edgeX-Api-Key", api_key_},
            {"X-edgeX-Passphrase", passphrase_},
            {"X-edgeX-Signature", std::string{hmac_hex_.data(), hmac_hex_.size()}},
            {"X-edgeX-Timestamp", std::string{timestamp_.data(), timestamp_len_}}};
}

std::string RequestBuilder::stream_target() const {
    std::string out{k_private_ws};
    out.append("?accountId=")
        .append(account_text_.data(), account_len_)
        .append("&timestamp=")
        .append(timestamp_.data(), timestamp_len_);
    return out;
}

std::expected<WireReply, WireError> parse_reply(std::string_view body) {
    return guarded([&]() -> std::expected<WireReply, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireReply out{};
        const auto read = json::for_each_field(*root, [&](std::string_view key, json::ondemand::value& value)
                                                          -> std::expected<void, WireError> {
            if (key == "code") {
                out.code = text_of(value);
            } else if (key == "msg") {
                out.msg = text_of(value);
            } else if (key == "data") {
                json::ondemand::object data;
                if (value.get_object().get(data) != simdjson::SUCCESS) {
                    return {}; // null or a list carries no verdict detail
                }
                return json::for_each_field(data, [&](std::string_view field, json::ondemand::value& v)
                                                      -> std::expected<void, WireError> {
                    if (field == "orderId") {
                        out.order_id = text_of(v);
                        return {};
                    }
                    json::ondemand::object results;
                    if (field != "cancelResultMap" || v.get_object().get(results) != simdjson::SUCCESS) {
                        return {};
                    }
                    return json::for_each_field(results, [&](std::string_view id, json::ondemand::value& r) {
                        if (out.order_id.empty()) {
                            out.order_id = id;
                            out.cancel_result = text_of(r);
                        }
                    });
                });
            }
            return {};
        });
        if (!read.has_value()) {
            return std::unexpected(read.error());
        }
        if (out.code.empty()) {
            return std::unexpected(WireError::MissingField);
        }
        return out;
    });
}

ErrorClass classify(int http_status, std::string_view code) noexcept {
    if (http_status == 429 || code == "RATE_LIMIT_EXCEEDED") {
        return ErrorClass::RateLimited;
    }
    if (http_status == 401 || http_status == 403 || code == "INVALID_API_KEY" || code == "SIGNATURE_ERROR" ||
        code == "GATEWAY_HEADER_REQUIRED" || code.find("SIGNATURE") != std::string_view::npos) {
        return ErrorClass::AuthFailure;
    }
    if (http_status >= 500) {
        return ErrorClass::Retryable;
    }
    return ErrorClass::Terminal;
}

bool is_benign_cancel_failure(std::string_view code) noexcept {
    return code == "ORDER_NOT_FOUND" || code == "ORDER_NOT_EXIST" || code == "ORDER_ALREADY_FINISHED";
}

std::expected<PrivateFrame, WireError> parse_private_kind(std::string_view frame) {
    return guarded([&]() -> std::expected<PrivateFrame, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto type = json::req_str(*root, "type");
        if (!type.has_value()) {
            return std::unexpected(type.error());
        }
        if (*type == "connected") {
            return PrivateFrame{.kind = PrivateKind::Connected};
        }
        if (*type == "ping") {
            return PrivateFrame{.kind = PrivateKind::Ping};
        }
        if (*type == "error") {
            return PrivateFrame{.kind = PrivateKind::Error};
        }
        if (*type != "trade-event") {
            return PrivateFrame{};
        }
        auto content = json::req_object(*root, "content");
        if (!content.has_value()) {
            return std::unexpected(content.error());
        }
        const auto event = json::req_str(*content, "event");
        if (!event.has_value()) {
            return std::unexpected(event.error());
        }
        return PrivateFrame{.kind = PrivateKind::Trade, .event = *event};
    });
}

std::expected<TradeRows, WireError> parse_trade_event(std::string_view frame, TradeBuffer& storage) {
    return guarded([&]() -> std::expected<TradeRows, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto content = json::req_object(*root, "content");
        if (!content.has_value()) {
            return std::unexpected(content.error());
        }
        auto data = json::req_object(*content, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        return read_groups(*data, storage);
    });
}

std::expected<TradeRows, WireError> parse_rest_rows(std::string_view body, TradeBuffer& storage, bool fills) {
    return guarded([&]() -> std::expected<TradeRows, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = success_data(*root);
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        json::ondemand::json_type type{};
        if (const auto ec = data->type().get(type); ec != simdjson::SUCCESS) {
            return std::unexpected(json::map_error(ec));
        }
        json::ondemand::value list = *data;
        json::ondemand::object page;
        if (type != json::ondemand::json_type::array) {
            if (const auto ec = data->get_object().get(page); ec != simdjson::SUCCESS) {
                return std::unexpected(json::map_error(ec));
            }
            if (const auto ec = json::find(page, "dataList").get(list); ec != simdjson::SUCCESS) {
                return std::unexpected(json::map_error(ec));
            }
        }
        if (fills) {
            const auto count = read_rows(list, storage.fills, fill_fill);
            if (!count.has_value()) {
                return std::unexpected(count.error());
            }
            return TradeRows{.fills = {storage.fills.data(), *count}};
        }
        const auto count = read_rows(list, storage.orders, fill_order);
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        return TradeRows{.orders = {storage.orders.data(), *count}};
    });
}

std::expected<TradeRows, WireError> parse_account_asset(std::string_view body, TradeBuffer& storage) {
    return guarded([&]() -> std::expected<TradeRows, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto value = success_data(*root);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        json::ondemand::object data;
        if (const auto ec = value->get_object().get(data); ec != simdjson::SUCCESS) {
            return std::unexpected(json::map_error(ec));
        }
        return read_groups(data, storage);
    });
}

std::optional<OrderEvent> map_order(const WireOrder& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto side = side_of(wire.side);
    const auto size = lots_of(wire.size, base);
    const auto matched = lots_of(wire.cum_match_size, base);
    if (!side.has_value() || !size.has_value() || !matched.has_value()) {
        return std::nullopt;
    }
    OrderEvent out{};
    if (wire.status == "OPEN" && matched->raw == 0) {
        out.type = OrderEventType::Accepted;
        out.leaves_qty = *size;
    } else if (wire.status == "CANCELED") {
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
    const auto matched = lots_of(wire.cum_match_size, base);
    const auto client = ExchangeClientOrderId::from(wire.client_order_id);
    const auto id = ExchangeOrderId::from(wire.id);
    if (!side.has_value() || !size.has_value() || !matched.has_value() || !client.has_value() || !id.has_value()) {
        return std::nullopt;
    }
    exec::OrderStatusReport out{};
    out.exchange_client_order_id = *client;
    out.exchange_order_id = *id;
    out.instrument = base.id;
    out.side = *side;
    out.qty = *size;
    out.leaves = QtyLots::from_raw(size->raw > matched->raw ? size->raw - matched->raw : 0);
    if (wire.status == "FILLED") {
        out.status = OrderStatus::Filled;
        out.leaves = QtyLots::from_raw(0);
    } else if (wire.status == "CANCELED") {
        out.status = OrderStatus::Canceled;
        out.leaves = QtyLots::from_raw(0);
    } else if (wire.status == "CANCELING") {
        out.status = OrderStatus::PendingCancel;
    } else if (matched->raw > 0) {
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

std::optional<OrderEvent> map_fill(const WireFill& wire, const InstrumentBase& base, QtyLots leaves, UnixNanos ts_init) {
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
    out.liquidity = wire.direction == "MAKER"   ? Liquidity::Maker
                    : wire.direction == "TAKER" ? Liquidity::Taker
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
    // positive is paid, as on the other venues
    if (const auto fee = parse_decimal_e9(wire.fee); fee.has_value()) {
        out.fee = *fee;
    }
    out.ts_event = from_ms(wire.match_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<exec::PositionReport> map_position(const WirePosition& wire, const InstrumentBase& base) {
    auto size = wire.open_size;
    const bool short_side = size.starts_with('-');
    if (short_side) {
        size.remove_prefix(1);
    }
    const auto lots = lots_of(size, base);
    if (!lots.has_value()) {
        return std::nullopt;
    }
    exec::PositionReport out{};
    out.instrument = base.id;
    out.net_qty = short_side ? QtyLots::from_raw(-lots->raw) : *lots;
    // openValue over openSize is the entry; off the grid it is left undefined
    auto value = wire.open_value;
    if (value.starts_with('-')) {
        value.remove_prefix(1);
    }
    const auto notional = parse_decimal_e9(value);
    const auto qty = base.lots_to_qty(*lots);
    if (notional.has_value() && qty.raw > 0) {
        const auto scaled = static_cast<__int128>(notional->raw) * 1'000'000'000;
        if (scaled % qty.raw == 0) {
            const auto entry = FixedE9::from_raw(static_cast<std::int64_t>(scaled / qty.raw));
            if (const auto ticks = base.price_to_ticks(entry); ticks.has_value()) {
                out.avg_entry = *ticks;
            }
        }
    }
    return out;
}

} // namespace raikiri::venues::edgex
