#include <raikiri/net/json_writer.hpp>
#include <raikiri/venues/bitmex/rest.hpp>

#include <charconv>
#include <cstring>
#include <span>

namespace raikiri::venues::bitmex {

namespace {

constexpr std::string_view k_host_live = "www.bitmex.com";
constexpr std::string_view k_host_test = "testnet.bitmex.com";

constexpr std::string_view k_path_order = "/api/v1/order";
constexpr std::string_view k_path_trade_history = "/api/v1/execution/tradeHistory";
constexpr std::string_view k_path_position = "/api/v1/position";

constexpr std::string_view k_hdr_key = "api-key";
constexpr std::string_view k_hdr_expires = "api-expires";
constexpr std::string_view k_hdr_signature = "api-signature";

// Fills the builder's target buffer. A part that would overflow is dropped
// rather than truncated mid-token, so an over-long symbol yields a request
// BitMEX refuses with a 400 instead of one that names a different instrument.
class TargetWriter {
public:
    explicit TargetWriter(std::span<char> into) noexcept : into_{into} {}

    void put(std::string_view text) noexcept {
        if (len_ + text.size() > into_.size()) {
            return;
        }
        std::memcpy(into_.data() + len_, text.data(), text.size());
        len_ += text.size();
    }

    void put(std::uint32_t value) noexcept {
        std::array<char, 16> num{};
        if (const auto [stop, ec] = std::to_chars(num.data(), num.data() + num.size(), value);
            ec == std::errc{}) {
            put(std::string_view{num.data(), static_cast<std::size_t>(stop - num.data())});
        }
    }

    [[nodiscard]] net::RestRequest get() const noexcept {
        return {.method = net::HttpMethod::Get,
                .target = std::string_view{into_.data(), len_},
                .body = {}};
    }

private:
    std::span<char> into_;
    std::size_t len_{0};
};

// Appends into a fixed buffer, reporting overflow rather than truncating: the
// path is a TERM OF THE SIGNATURE here, so a truncated one authenticates a
// request nobody wrote.
class Appender {
public:
    explicit Appender(std::span<char> buf) noexcept : buf_{buf} {}

    Appender& operator<<(std::string_view text) noexcept {
        if (overflow_ || len_ + text.size() > buf_.size()) {
            overflow_ = true;
            return *this;
        }
        std::memcpy(buf_.data() + len_, text.data(), text.size());
        len_ += text.size();
        return *this;
    }

    Appender& operator<<(std::uint64_t value) noexcept {
        std::array<char, 24> num{};
        const auto [stop, ec] = std::to_chars(num.data(), num.data() + num.size(), value);
        if (ec != std::errc{}) {
            overflow_ = true;
            return *this;
        }
        return *this << std::string_view{num.data(), static_cast<std::size_t>(stop - num.data())};
    }

    // Percent-encodes everything outside the unreserved set. The filter values
    // BitMEX takes are JSON — braces, quotes and colons — none of which may
    // travel raw in a query, and the encoded form is what gets signed.
    Appender& encoded(std::string_view text) noexcept {
        constexpr std::string_view k_hex = "0123456789ABCDEF";
        for (const char c : text) {
            const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                    (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
                                    c == '~';
            if (unreserved) {
                *this << std::string_view{&c, 1};
                continue;
            }
            const auto byte = static_cast<std::uint8_t>(c);
            const std::array<char, 3> escape{'%', k_hex[byte >> 4U], k_hex[byte & 0x0FU]};
            *this << std::string_view{escape.data(), escape.size()};
        }
        return *this;
    }

    [[nodiscard]] bool ok() const noexcept { return !overflow_; }

    [[nodiscard]] std::string_view view() const noexcept {
        return overflow_ ? std::string_view{} : std::string_view{buf_.data(), len_};
    }

private:
    std::span<char> buf_;
    std::size_t len_{0};
    bool overflow_{false};
};

[[nodiscard]] std::string_view method_text(net::HttpMethod method) noexcept {
    switch (method) {
    case net::HttpMethod::Get:
        return "GET";
    case net::HttpMethod::Post:
        return "POST";
    case net::HttpMethod::Put:
        return "PUT";
    case net::HttpMethod::Delete:
        return "DELETE";
    case net::HttpMethod::Patch:
        return "PATCH";
    }
    return "GET";
}

// Days since 1970-01-01 -> proleptic Gregorian y/m/d (Hinnant's
// civil_from_days). gmtime_r would answer the same question through a
// locale-aware path that is neither noexcept nor free of global state.
struct Civil {
    std::int64_t year{0};
    unsigned month{0};
    unsigned day{0};
};

[[nodiscard]] Civil civil_from_days(std::int64_t z) noexcept {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<std::uint64_t>(z - era * 146097);
    const std::uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const std::uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::uint64_t mp = (5 * doy + 2) / 153;
    const std::uint64_t d = doy - (153 * mp + 2) / 5 + 1;
    const std::uint64_t m = mp < 10 ? mp + 3 : mp - 9;
    return {.year = y + static_cast<std::int64_t>(m <= 2),
            .month = static_cast<unsigned>(m),
            .day = static_cast<unsigned>(d)};
}

void put_two(char* out, unsigned value) noexcept {
    out[0] = static_cast<char>('0' + (value / 10) % 10);
    out[1] = static_cast<char>('0' + value % 10);
}

// "2026-08-26T08:00:00.000Z" — the same 24-character form the venue itself
// sends and map.cpp reads back. Empty outside the four-digit year BitMEX
// parses, so a nonsense window is never sent as a plausible one.
[[nodiscard]] std::string_view iso8601(UnixNanos when, std::span<char> out) noexcept {
    constexpr std::size_t k_len = 24;
    if (out.size() < k_len) {
        return {};
    }
    const auto ms = static_cast<std::int64_t>(when.ns / 1'000'000ULL);
    const std::int64_t seconds = ms / 1'000;
    const auto milli = static_cast<unsigned>(ms % 1'000);
    const std::int64_t days = seconds / 86'400;
    auto rem = seconds % 86'400;
    const auto civil = civil_from_days(days);
    if (civil.year < 1000 || civil.year > 9999) {
        return {};
    }

    char* p = out.data();
    put_two(p, static_cast<unsigned>(civil.year / 100));
    put_two(p + 2, static_cast<unsigned>(civil.year % 100));
    p[4] = '-';
    put_two(p + 5, civil.month);
    p[7] = '-';
    put_two(p + 8, civil.day);
    p[10] = 'T';
    put_two(p + 11, static_cast<unsigned>(rem / 3600));
    p[13] = ':';
    rem %= 3600;
    put_two(p + 14, static_cast<unsigned>(rem / 60));
    p[16] = ':';
    put_two(p + 17, static_cast<unsigned>(rem % 60));
    p[19] = '.';
    p[20] = static_cast<char>('0' + (milli / 100) % 10);
    put_two(p + 21, milli % 100);
    p[23] = 'Z';
    return {out.data(), k_len};
}

// BitMEX names order type and time in force in two fields, so both are sent as
// asked. A post-only IOC is a contradiction the VENUE refuses by name — better
// than this quietly dropping half of what the strategy said.
[[nodiscard]] std::string_view ord_type_text(const OrderCmd& cmd) noexcept {
    return cmd.order_type == OrderType::Market ? "Market" : "Limit";
}

[[nodiscard]] std::string_view time_in_force_text(const OrderCmd& cmd) noexcept {
    switch (cmd.tif) {
    case TimeInForce::Ioc:
        return "ImmediateOrCancel";
    case TimeInForce::Fok:
        return "FillOrKill";
    case TimeInForce::Gtc:
        break;
    }
    // A market order never rests, and BitMEX refuses GoodTillCancel on one.
    return cmd.order_type == OrderType::Market ? "ImmediateOrCancel" : "GoodTillCancel";
}

// `execInst` is a comma-separated LIST, and the two flags this codebase carries
// are the only members ever produced, so the three combinations are constants
// rather than a join over a buffer.
[[nodiscard]] std::string_view exec_inst_text(OrderFlags flags) noexcept {
    const bool post_only = has_flag(flags, OrderFlags::PostOnly);
    const bool reduce_only = has_flag(flags, OrderFlags::ReduceOnly);
    if (post_only && reduce_only) {
        return "ParticipateDoNotInitiate,ReduceOnly";
    }
    if (post_only) {
        return "ParticipateDoNotInitiate";
    }
    if (reduce_only) {
        return "ReduceOnly";
    }
    return {};
}

// `orderQty` is a JSON INTEGER in the venue's own units — contracts on a
// derivative, base minor units on spot — which is exactly what lot_size counts
// in. A quantity that is not a whole number of them cannot be sent, and must
// not be rounded into one that can.
[[nodiscard]] std::optional<std::int64_t> whole_units(QtyLots lots,
                                                      const InstrumentBase& base) noexcept {
    const auto qty = base.lots_to_qty(lots);
    if (qty.is_undef() || qty.raw <= 0 || qty.raw % k_e9 != 0) {
        return std::nullopt;
    }
    return qty.raw / k_e9;
}

[[nodiscard]] std::optional<FixedE9> order_price(const OrderCmd& cmd,
                                                 const InstrumentBase& base) noexcept {
    const auto price = base.ticks_to_price(cmd.price);
    if (price.is_undef() || price.raw <= 0) {
        return std::nullopt;
    }
    return price;
}

} // namespace

std::string_view rest_host(bool testnet) noexcept {
    return testnet ? k_host_test : k_host_live;
}

std::string_view to_string(BuildError err) noexcept {
    switch (err) {
    case BuildError::UnknownInstrument:
        return "unknown instrument";
    case BuildError::BadQuantity:
        return "bad quantity";
    case BuildError::BadPrice:
        return "bad price";
    case BuildError::BufferTooSmall:
        return "buffer too small";
    case BuildError::NotSigned:
        return "credentials not loaded";
    case BuildError::NoOrderTarget:
        return "no order id and no client order id";
    case BuildError::EmptyBatch:
        return "cancel batch names no orders";
    case BuildError::BatchTooLarge:
        return "cancel batch larger than the venue accepts";
    case BuildError::DatedFuturesUnsupported:
        return "bitmex dated futures order entry";
    case BuildError::OptionsUnsupported:
        return "bitmex options order entry";
    }
    return "unknown";
}

// Spot and the perpetuals are both served. The other two markets get their OWN
// name so a refusal says which surface is missing rather than blaming the
// catalogue.
std::optional<BuildError> market_refusal(MarketType market) noexcept {
    switch (market) {
    case MarketType::Spot:
    case MarketType::Perp:
        return std::nullopt;
    case MarketType::Future:
        return BuildError::DatedFuturesUnsupported;
    case MarketType::Option:
        return BuildError::OptionsUnsupported;
    }
    return BuildError::OptionsUnsupported;
}

// Every builder below writes into the same buffer, so a request must be sent
// before the next one is built. One catalogue load at a time is the only use.
net::RestRequest RequestBuilder::instruments() noexcept {
    TargetWriter writer{target_};
    writer.put("/api/v1/instrument/active");
    return writer.get();
}

net::RestRequest RequestBuilder::instrument(std::string_view symbol) noexcept {
    TargetWriter writer{target_};
    writer.put("/api/v1/instrument?symbol=");
    writer.put(symbol);
    return writer.get();
}

net::RestRequest RequestBuilder::book(std::string_view symbol, std::uint32_t depth) noexcept {
    TargetWriter writer{target_};
    writer.put("/api/v1/orderBook/L2?symbol=");
    writer.put(symbol);
    writer.put("&depth=");
    writer.put(depth);
    return writer.get();
}

net::RestRequest RequestBuilder::trades(std::string_view symbol, std::uint32_t count) noexcept {
    TargetWriter writer{target_};
    writer.put("/api/v1/trade?symbol=");
    writer.put(symbol);
    writer.put("&reverse=true&count=");
    writer.put(count);
    return writer.get();
}

// -------------------------------------------------------------- signed ----

std::string_view SignedRequestBuilder::last_sign_message() const noexcept {
    return {sign_msg_.data(), sign_msg_len_};
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::sign(net::HttpMethod method,
                                                                       std::string_view path,
                                                                       std::string_view body,
                                                                       UnixNanos now) {
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NotSigned};
    }
    Appender expires{expires_};
    expires << now.ns / 1'000'000'000ULL + k_expiry_window_s;
    if (!expires.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }

    // verb + path + expires + body, bare concatenation. The path already
    // carries its query, percent-encoded exactly as it will travel.
    Appender msg{sign_msg_};
    msg << method_text(method) << path << expires.view() << body;
    if (!msg.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    sign_msg_len_ = msg.view().size();

    const auto signature = signer_->sign(msg.view(), net::Signer::Encoding::Hex, signature_);
    if (signature.empty()) {
        return std::unexpected{BuildError::NotSigned};
    }

    net::RestRequest request{};
    request.method = method;
    request.target = path;
    request.body = body;
    request.headers = {{k_hdr_key, signer_->api_key()},
                       {k_hdr_expires, expires.view()},
                       {k_hdr_signature, signature}};
    return request;
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::create_order(
    const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
    if (const auto refusal = market_refusal(cmd.instrument.market()); refusal.has_value()) {
        return std::unexpected{*refusal};
    }
    const auto* instrument = table.find(cmd.instrument);
    if (instrument == nullptr) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    const auto& base = base_of(*instrument);
    const auto qty = whole_units(cmd.qty, base);
    if (!qty.has_value()) {
        return std::unexpected{BuildError::BadQuantity};
    }

    net::JsonWriter json{body_};
    json.obj_begin()
        .str("symbol", base.exchange_symbol.view())
        .str("side", cmd.side == Side::Buy ? "Buy" : "Sell")
        .num("orderQty", *qty)
        .str("ordType", ord_type_text(cmd));
    if (cmd.order_type != OrderType::Market) {
        // BitMEX defaults ordType to 'Limit' when a price is present and
        // 'Market' when it is not; sending a price on a market order would
        // silently change which order was placed.
        const auto price = order_price(cmd, base);
        if (!price.has_value()) {
            return std::unexpected{BuildError::BadPrice};
        }
        json.decimal_raw("price", *price);
    }
    json.str("timeInForce", time_in_force_text(cmd));
    if (const auto exec_inst = exec_inst_text(cmd.flags); !exec_inst.empty()) {
        json.str("execInst", exec_inst);
    }
    if (!cmd.exchange_client_order_id.empty()) {
        json.str("clOrdID", cmd.exchange_client_order_id.view());
    }
    json.obj_end();
    if (!json.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Post, k_path_order, json.view(), now);
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::cancel_order(
    const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
    if (const auto refusal = market_refusal(cmd.instrument.market()); refusal.has_value()) {
        return std::unexpected{*refusal};
    }
    if (table.find(cmd.instrument) == nullptr) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    net::JsonWriter json{body_};
    json.obj_begin();
    // The venue's own id is preferred: a clOrdID is unique only among orders
    // still working, so cancelling by it races an id the OMS has reused.
    if (!cmd.target_exchange_order_id.empty()) {
        json.str("orderID", cmd.target_exchange_order_id.view());
    } else if (!cmd.exchange_client_order_id.empty()) {
        json.str("clOrdID", cmd.exchange_client_order_id.view());
    } else {
        return std::unexpected{BuildError::NoOrderTarget};
    }
    json.obj_end();
    if (!json.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    // A DELETE carries a body here, and that body is part of the digest.
    return sign(net::HttpMethod::Delete, k_path_order, json.view(), now);
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::amend_order(
    const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
    if (const auto refusal = market_refusal(cmd.instrument.market()); refusal.has_value()) {
        return std::unexpected{*refusal};
    }
    const auto* instrument = table.find(cmd.instrument);
    if (instrument == nullptr) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    const auto& base = base_of(*instrument);
    const auto qty = whole_units(cmd.qty, base);
    const auto price = order_price(cmd, base);
    if (!qty.has_value()) {
        return std::unexpected{BuildError::BadQuantity};
    }
    if (!price.has_value()) {
        // PUT /order has no market form: an amend is always to a resting price.
        return std::unexpected{BuildError::BadPrice};
    }

    net::JsonWriter json{body_};
    json.obj_begin();
    if (!cmd.target_exchange_order_id.empty()) {
        json.str("orderID", cmd.target_exchange_order_id.view());
    } else if (!cmd.exchange_client_order_id.empty()) {
        // `origClOrdID`, not `clOrdID`: the latter would RENAME the order.
        json.str("origClOrdID", cmd.exchange_client_order_id.view());
    } else {
        return std::unexpected{BuildError::NoOrderTarget};
    }
    json.num("orderQty", *qty).decimal_raw("price", *price).obj_end();
    if (!json.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Put, k_path_order, json.view(), now);
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::cancel_batch(
    std::span<const OrderCmd> cmds, const exec::InstrumentTable& table, UnixNanos now) {
    if (cmds.empty()) {
        return std::unexpected{BuildError::EmptyBatch};
    }
    if (cmds.size() > k_max_batch_cancel) {
        return std::unexpected{BuildError::BatchTooLarge};
    }
    Appender ids{ids_};
    ids << "[";
    for (std::size_t i = 0; i < cmds.size(); ++i) {
        const auto& cmd = cmds[i];
        if (const auto refusal = market_refusal(cmd.instrument.market()); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (table.find(cmd.instrument) == nullptr) {
            return std::unexpected{BuildError::UnknownInstrument};
        }
        if (cmd.target_exchange_order_id.empty()) {
            // The array form takes venue ids only; a batch is all-or-nothing
            // rather than silently dropping the ones we cannot name.
            return std::unexpected{BuildError::NoOrderTarget};
        }
        if (i != 0) {
            ids << ",";
        }
        ids << "\"" << cmd.target_exchange_order_id.view() << "\"";
    }
    ids << "]";
    if (!ids.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    net::JsonWriter json{body_};
    json.obj_begin().raw("orderID", ids.view()).obj_end();
    if (!json.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Delete, k_path_order, json.view(), now);
}

std::expected<net::RestRequest, BuildError>
SignedRequestBuilder::open_orders(std::uint32_t start, UnixNanos now) {
    Appender path{target_};
    path << k_path_order << "?filter=";
    path.encoded(R"({"open":true})");
    path << "&count=" << static_cast<std::uint64_t>(k_page_size) << "&reverse=true&start="
         << static_cast<std::uint64_t>(start);
    if (!path.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Get, path.view(), {}, now);
}

std::expected<net::RestRequest, BuildError>
SignedRequestBuilder::query_order(const ExchangeClientOrderId& id, UnixNanos now) {
    if (id.empty()) {
        return std::unexpected{BuildError::NoOrderTarget};
    }
    // The filter is built into `body_` first because the encoded form is what
    // the path — and therefore the signature — must carry.
    Appender filter{body_};
    filter << R"({"clOrdID":")" << id.view() << R"("})";
    if (!filter.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    Appender path{target_};
    path << k_path_order << "?filter=";
    path.encoded(filter.view());
    path << "&count=1&reverse=true";
    if (!path.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Get, path.view(), {}, now);
}

std::expected<net::RestRequest, BuildError>
SignedRequestBuilder::fills(UnixNanos since, std::uint32_t start, UnixNanos now) {
    Appender path{target_};
    path << k_path_trade_history << "?count=" << static_cast<std::uint64_t>(k_page_size)
         << "&reverse=true&start=" << static_cast<std::uint64_t>(start);
    if (since.ns != 0) {
        std::array<char, 32> stamp{};
        const auto text = iso8601(since, stamp);
        if (text.empty()) {
            return std::unexpected{BuildError::BufferTooSmall};
        }
        path << "&startTime=";
        path.encoded(text);
    }
    if (!path.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Get, path.view(), {}, now);
}

std::expected<net::RestRequest, BuildError> SignedRequestBuilder::positions(UnixNanos now) {
    Appender path{target_};
    path << k_path_position << "?count=" << static_cast<std::uint64_t>(k_page_size);
    if (!path.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return sign(net::HttpMethod::Get, path.view(), {}, now);
}

} // namespace raikiri::venues::bitmex
