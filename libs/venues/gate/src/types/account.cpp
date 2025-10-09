#include <raikiri/venues/gate/account.hpp>
#include <raikiri/venues/gate/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::gate {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson layer;
// a venue that reimplements opt_str or req_array is one that will disagree
// with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

std::expected<void, WireError> read_book_row(detail::ondemand::object& obj,
                                             WireAccountBookRow& out) {
    out.contract = field(obj, "contract");
    out.type = field(obj, "type");
    out.change = field(obj, "change");
    out.text = field(obj, "text");
    // A STRING on this endpoint, though `time` beside it is a number. Verified
    // against a live row: gate quotes the ledger's own id as "991", and
    // reading it as a number yields nothing.
    out.id = field(obj, "id");
    out.trade_id = field(obj, "trade_id");
    obj.reset();
    out.time_s = num_u64(obj, "time").value_or(0);
    return {};
}

} // namespace

std::expected<WireFuturesAccount, WireError> parse_futures_account(std::string_view body) {
    return guarded([&]() -> std::expected<WireFuturesAccount, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto currency = detail::req_str(*root, "currency");
        if (!currency.has_value()) {
            return std::unexpected(currency.error());
        }
        WireFuturesAccount out{};
        out.currency = *currency;
        out.total = field(*root, "total");
        out.available = field(*root, "available");
        out.unrealised_pnl = field(*root, "unrealised_pnl");
        out.update_s = whole_u64(field(*root, "update_time"));
        return out;
    });
}

std::expected<std::uint64_t, WireError> parse_account_user_id(std::string_view body) {
    return guarded([&]() -> std::expected<std::uint64_t, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto user = num_u64(*root, "user");
        if (!user.has_value()) {
            return std::unexpected(user.error());
        }
        if (*user == 0) {
            return std::unexpected(WireError::MissingField);
        }
        return *user;
    });
}

std::expected<std::span<const WireAccountBookRow>, WireError>
parse_account_book(std::string_view body, AccountBookBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireAccountBookRow>, WireError> {
        auto root = session().open_array(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return read_array<WireAccountBookRow>(*root, storage.rows, read_book_row);
    });
}

} // namespace raikiri::venues::gate
