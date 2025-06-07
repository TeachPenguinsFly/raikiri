#include "wire_detail.hpp"

namespace raikiri::venues::bitfinex {

namespace {

using detail::guarded;
using detail::session;

using JsonType = detail::ondemand::json_type;

// The socket wraps a payload as `[0,"<tag>",<payload>]` while REST answers the
// payload alone. Position 1 is TYPED, not counted — the same rule the public
// envelope reader follows — so a tag is a string there and a payload is not.
template <typename Fn>
[[nodiscard]] auto with_payload(std::string_view frame, Fn&& fn)
    -> decltype(fn(std::declval<detail::ondemand::value>())) {
    auto root = session().open_array(frame);
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    std::size_t index = 0;
    for (auto entry : *root) {
        detail::ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        JsonType type{};
        if (const auto ec = value.type().get(type); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        if (type == JsonType::array) {
            return std::forward<Fn>(fn)(value);
        }
        ++index;
        if (index > 2) {
            break; // a channel id and a tag are all that may precede a payload
        }
    }
    return std::unexpected(WireError::MissingField);
}

std::expected<WireOrder, WireError> read_order(detail::ondemand::value value) {
    const auto row = detail::read_row<k_order_arity>(value);
    if (!row.has_value()) {
        return std::unexpected(row.error());
    }
    WireOrder out{};
    out.id = row->template at<k_order_id>();
    out.symbol = row->template at<k_order_symbol>();
    out.amount = row->template at<k_order_amount>();
    out.amount_orig = row->template at<k_order_amount_orig>();
    out.type = row->template at<k_order_type>();
    out.price = row->template at<k_order_price>();
    out.price_avg = row->template at<k_order_price_avg>();
    out.status_text = row->template at<k_order_status>();
    out.status = order_status(out.status_text);
    (void)detail::token_to_i64(row->template at<k_order_cid>(), out.cid);
    (void)detail::token_to_i64(row->template at<k_order_flags>(), out.flags);
    out.mts_create = detail::digits_to_u64(row->template at<k_order_mts_create>());
    out.mts_update = detail::digits_to_u64(row->template at<k_order_mts_update>());
    if (out.id.empty() || out.symbol.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return out;
}

std::expected<WireExecution, WireError> read_execution(detail::ondemand::value value) {
    const auto row = detail::read_row<k_exec_arity>(value);
    if (!row.has_value()) {
        return std::unexpected(row.error());
    }
    WireExecution out{};
    out.id = row->template at<k_exec_id>();
    out.symbol = row->template at<k_exec_symbol>();
    out.order_id = row->template at<k_exec_order_id>();
    out.amount = row->template at<k_exec_amount>();
    out.price = row->template at<k_exec_price>();
    out.fee = row->template at<k_exec_fee>();
    out.fee_currency = row->template at<k_exec_fee_currency>();
    out.mts = detail::digits_to_u64(row->template at<k_exec_mts>());
    // MAKER is 1 or -1, not a boolean, and it is absent from neither frame —
    // but a zero would be neither, so an unreadable token leaves the fill's
    // liquidity Unknown rather than defaulting it to taker.
    std::int64_t maker = 0;
    if (detail::token_to_i64(row->template at<k_exec_maker>(), maker) && maker != 0) {
        out.maker = maker > 0;
        out.liquidity_known = true;
    }
    if (out.id.empty() || out.symbol.empty() || out.order_id.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return out;
}

std::expected<WirePosition, WireError> read_position(detail::ondemand::value value) {
    const auto row = detail::read_row<k_position_arity>(value);
    if (!row.has_value()) {
        return std::unexpected(row.error());
    }
    WirePosition out{};
    out.symbol = row->template at<k_position_symbol>();
    out.status = row->template at<k_position_status>();
    out.amount = row->template at<k_position_amount>();
    out.base_price = row->template at<k_position_base_price>();
    out.mts_update = detail::digits_to_u64(row->template at<k_position_mts_update>());
    if (out.symbol.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return out;
}

// The writes whose PAYLOAD is exactly one order. `oc_multi-req` carries a list
// instead, and descending into it as if it were a row would read a whole order
// array where a field belongs.
bool names_one_order(std::string_view type) noexcept {
    return type == "on-req" || type == "ou-req" || type == "oc-req";
}

std::expected<void, WireError>
assign_notification(std::size_t index, detail::ondemand::value value, WireNotification& out) {
    switch (index) {
    case k_notify_mts: {
        const auto token = detail::token_of(value);
        if (!token.has_value()) {
            return std::unexpected(token.error());
        }
        out.mts = detail::digits_to_u64(*token);
        return {};
    }
    case k_notify_type: {
        const auto token = detail::token_of(value);
        if (!token.has_value()) {
            return std::unexpected(token.error());
        }
        out.type = *token;
        return {};
    }
    case k_notify_payload: {
        if (!names_one_order(out.type)) {
            return {}; // a list, or nothing at all; neither is one order
        }
        const auto order = read_order(value);
        if (!order.has_value()) {
            return std::unexpected(order.error());
        }
        out.order = *order;
        out.has_order = true;
        return {};
    }
    case k_notify_code: {
        const auto token = detail::token_of(value);
        if (!token.has_value()) {
            return std::unexpected(token.error());
        }
        (void)detail::token_to_i64(*token, out.code);
        return {};
    }
    case k_notify_status: {
        const auto token = detail::token_of(value);
        if (!token.has_value()) {
            return std::unexpected(token.error());
        }
        out.status = *token;
        return {};
    }
    case k_notify_text: {
        const auto token = detail::token_of(value);
        if (!token.has_value()) {
            return std::unexpected(token.error());
        }
        out.text = *token;
        return {};
    }
    default:
        return {};
    }
}

std::expected<WireNotification, WireError> read_notification(detail::ondemand::array rows) {
    WireNotification out{};
    std::size_t index = 0;
    for (auto entry : rows) {
        detail::ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        if (const auto ok = assign_notification(index, value, out); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        ++index;
    }
    if (index < k_notify_arity) {
        return std::unexpected(WireError::ShortArray);
    }
    return out;
}

// Walks a list of positional rows, which arrives bare from REST and wrapped in
// a socket envelope on a snapshot.
template <typename Row, typename Read>
std::expected<std::size_t, WireError>
read_rows(detail::ondemand::array rows, std::span<Row> into, Read read) {
    std::size_t count = 0;
    for (auto entry : rows) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        detail::ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        const auto row = read(value);
        if (!row.has_value()) {
            return std::unexpected(row.error());
        }
        into[count] = *row;
        ++count;
    }
    return count;
}

// `[[` is a bare list of rows and `[0,` a socket envelope carrying one. Decided
// on the first two bytes for the same reason detail::is_array_frame is: it is
// the whole of the dispatch and the alternative is a rewind ondemand does not
// offer.
bool is_row_list(std::string_view frame) noexcept {
    const auto text = detail::trim(frame);
    if (text.size() < 2 || text.front() != '[') {
        return false;
    }
    const auto rest = detail::trim(text.substr(1));
    return !rest.empty() && rest.front() == '[';
}

template <typename Row, typename Read>
std::expected<std::size_t, WireError>
rows_of(std::string_view frame, std::span<Row> into, Read read) {
    if (is_row_list(frame)) {
        auto root = session().open_array(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return read_rows<Row>(*root, into, read);
    }
    return with_payload(
        frame, [&](detail::ondemand::value payload) -> std::expected<std::size_t, WireError> {
            detail::ondemand::array rows;
            if (const auto ec = payload.get_array().get(rows); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            return read_rows<Row>(rows, into, read);
        });
}

} // namespace

WireOrderStatus order_status(std::string_view text) noexcept {
    // Prefix matching, not equality: Bitfinex glues the fill onto the status
    // ("EXECUTED @ 63947.0(0.1)"), so == matches only ACTIVE and CANCELED and
    // silently drops every completed order into Unknown.
    if (text.starts_with("ACTIVE")) {
        return WireOrderStatus::Active;
    }
    if (text.starts_with("PARTIALLY FILLED")) {
        return WireOrderStatus::PartiallyFilled;
    }
    if (text.starts_with("EXECUTED")) {
        return WireOrderStatus::Executed;
    }
    // "POSTONLY CANCELED" does not START with CANCELED, and reading it as
    // anything else leaves a post-only rejection sitting open forever.
    if (text.starts_with("CANCELED") || text.starts_with("POSTONLY CANCELED")) {
        return WireOrderStatus::Canceled;
    }
    if (text.starts_with("INSUFFICIENT") || text.starts_with("RSN_")) {
        return WireOrderStatus::Rejected;
    }
    return WireOrderStatus::Unknown;
}

bool notification_ok(const WireNotification& notification) noexcept {
    // Bitfinex answers HTTP 200 to a refused order; this field is the verdict.
    return notification.status == "SUCCESS";
}

std::expected<WireAuthAck, WireError> parse_auth_ack(std::string_view frame) {
    return guarded([&]() -> std::expected<WireAuthAck, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto status = detail::opt_str(*root, "status");
        const auto msg = detail::opt_str(*root, "msg");
        const auto code = detail::opt_i64(*root, "code");
        if (!status.has_value() || !msg.has_value() || !code.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        return WireAuthAck{.ok = *status == "OK", .code = *code, .msg = *msg};
    });
}

std::expected<WireOrder, WireError> parse_order(std::string_view frame) {
    return guarded([&]() -> std::expected<WireOrder, WireError> {
        return with_payload(frame,
                            [](detail::ondemand::value payload) { return read_order(payload); });
    });
}

std::expected<std::span<const WireOrder>, WireError> parse_orders(std::string_view body,
                                                                  OrderBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireOrder>, WireError> {
        const auto count = rows_of<WireOrder>(
            body, storage.orders, [](detail::ondemand::value value) { return read_order(value); });
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        return std::span<const WireOrder>{storage.orders.data(), *count};
    });
}

std::expected<WireExecution, WireError> parse_execution(std::string_view frame) {
    return guarded([&]() -> std::expected<WireExecution, WireError> {
        return with_payload(
            frame, [](detail::ondemand::value payload) { return read_execution(payload); });
    });
}

std::expected<std::span<const WireExecution>, WireError>
parse_executions(std::string_view body, ExecutionBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireExecution>, WireError> {
        const auto count =
            rows_of<WireExecution>(body, storage.executions, [](detail::ondemand::value value) {
                return read_execution(value);
            });
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        return std::span<const WireExecution>{storage.executions.data(), *count};
    });
}

std::expected<WirePosition, WireError> parse_position(std::string_view frame) {
    return guarded([&]() -> std::expected<WirePosition, WireError> {
        return with_payload(frame,
                            [](detail::ondemand::value payload) { return read_position(payload); });
    });
}

std::expected<std::span<const WirePosition>, WireError> parse_positions(std::string_view body,
                                                                        PositionBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WirePosition>, WireError> {
        const auto count =
            rows_of<WirePosition>(body, storage.positions, [](detail::ondemand::value value) {
                return read_position(value);
            });
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        return std::span<const WirePosition>{storage.positions.data(), *count};
    });
}

std::expected<WireNotification, WireError> parse_notification(std::string_view body) {
    return guarded([&]() -> std::expected<WireNotification, WireError> {
        auto root = session().open_array(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireNotification out{};
        std::size_t index = 0;
        bool wrapped = false;
        for (auto entry : *root) {
            detail::ondemand::value value;
            if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            if (index == 1) {
                const auto token = detail::token_of(value);
                if (!token.has_value()) {
                    return std::unexpected(token.error());
                }
                // "n" is the socket TAG, never a notification type — every
                // type Bitfinex mints ends in "-req" or begins "ucm-" — so it
                // is what tells a wrapped frame from a bare REST body.
                if (*token == "n") {
                    wrapped = true;
                    ++index;
                    continue;
                }
                out.type = *token;
                ++index;
                continue;
            }
            if (wrapped && index == 2) {
                detail::ondemand::array rows;
                if (const auto ec = value.get_array().get(rows); ec != simdjson::SUCCESS) {
                    return std::unexpected(detail::map_error(ec));
                }
                return read_notification(rows);
            }
            if (const auto ok = assign_notification(index, value, out); !ok.has_value()) {
                return std::unexpected(ok.error());
            }
            ++index;
        }
        if (index < k_notify_arity) {
            return std::unexpected(WireError::ShortArray);
        }
        return out;
    });
}

} // namespace raikiri::venues::bitfinex
