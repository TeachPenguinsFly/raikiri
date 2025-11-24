#include <raikiri/venues/gate/catalogue.hpp>
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

std::expected<void, WireError> read_currency_pair(detail::ondemand::object& obj,
                                                  WireCurrencyPair& out) {
    const auto id = detail::req_str(obj, "id");
    if (!id.has_value()) {
        return std::unexpected{id.error()};
    }
    out.id = *id;
    out.trade_status = text_field(obj, "trade_status");
    out.min_base_amount = field(obj, "min_base_amount");
    out.min_quote_amount = field(obj, "min_quote_amount");
    out.precision = static_cast<std::uint32_t>(whole_u64(field(obj, "precision")));
    out.amount_precision = static_cast<std::uint32_t>(whole_u64(field(obj, "amount_precision")));
    return {};
}

} // namespace

std::expected<std::span<const WireContract>, WireError> parse_contracts(std::string_view body,
                                                                        ContractBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireContract>, WireError> {
        auto root = session().open_array(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        std::size_t count = 0;
        for (auto entry : *root) {
            if (count == k_max_contracts) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WireContract out{};
            const auto name = detail::req_str(obj, "name");
            if (!name.has_value()) {
                return std::unexpected(name.error());
            }
            out.name = *name;
            const auto status = detail::opt_str(obj, "status");
            const auto tick = opt_num(obj, "order_price_round");
            const auto size_min = opt_num(obj, "order_size_min");
            const auto quanto = opt_num(obj, "quanto_multiplier");
            const auto mark = opt_num(obj, "mark_price");
            const auto rate = opt_num(obj, "funding_rate");
            const auto interval = num_u64(obj, "funding_interval");
            const auto next_apply = num_u64(obj, "funding_next_apply");
            const auto delisting = detail::opt_bool(obj, "in_delisting");
            if (!status.has_value() || !tick.has_value() || !size_min.has_value() ||
                !quanto.has_value() || !mark.has_value() || !rate.has_value() ||
                !interval.has_value() || !next_apply.has_value() || !delisting.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.status = *status;
            out.order_price_round = *tick;
            out.order_size_min = *size_min;
            out.quanto_multiplier = *quanto;
            out.mark_price = *mark;
            out.funding_rate = *rate;
            out.funding_interval_s = static_cast<std::uint32_t>(*interval);
            out.funding_rate_limit =
                detail::opt_str(obj, "funding_rate_limit").value_or(std::string_view{});
            out.funding_next_apply_s = *next_apply;
            out.in_delisting = *delisting;
            storage.contracts[count] = out;
            ++count;
        }
        return std::span<const WireContract>{storage.contracts.data(), count};
    });
}

std::expected<std::span<const WireCurrencyPair>, WireError>
parse_currency_pairs(std::string_view body, CurrencyPairBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireCurrencyPair>, WireError> {
        auto root = session().open_array(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return read_array<WireCurrencyPair>(*root, storage.pairs, read_currency_pair);
    });
}

} // namespace raikiri::venues::gate
