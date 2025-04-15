#include "wire_detail.hpp"

namespace raikiri::venues::gate {

namespace {

using detail::guarded;
using detail::session;

} // namespace

std::expected<RestError, WireError> parse_rest_error(std::string_view body) {
    return guarded([&]() -> std::expected<RestError, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto label = detail::req_str(*root, "label");
        if (!label.has_value()) {
            return std::unexpected(label.error());
        }
        // Absent on CONTRACT_NOT_FOUND, present on most others. An empty
        // message is not a reason to discard a label we can classify.
        const auto message = detail::opt_str(*root, "message");
        if (!message.has_value()) {
            return std::unexpected(message.error());
        }
        return RestError{.label = *label, .message = *message};
    });
}

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
            // Optional at the wire layer: the mapper is what refuses a row,
            // and reporting exactly what the venue said is this layer's job.
            const auto status = detail::opt_str(obj, "status");
            const auto tick = detail::opt_num(obj, "order_price_round");
            const auto size_min = detail::opt_num(obj, "order_size_min");
            const auto quanto = detail::opt_num(obj, "quanto_multiplier");
            const auto mark = detail::opt_num(obj, "mark_price");
            const auto rate = detail::opt_num(obj, "funding_rate");
            const auto interval = detail::opt_u64(obj, "funding_interval");
            const auto next_apply = detail::opt_u64(obj, "funding_next_apply");
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
            out.funding_next_apply_s = *next_apply;
            out.in_delisting = *delisting;
            storage.contracts[count] = out;
            ++count;
        }
        return std::span<const WireContract>{storage.contracts.data(), count};
    });
}

std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body) {
    return guarded([&]() -> std::expected<std::uint64_t, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto ms = detail::opt_u64(*root, "server_time");
        if (!ms.has_value()) {
            return std::unexpected(ms.error());
        }
        if (*ms == 0) {
            return std::unexpected(WireError::MissingField);
        }
        return *ms;
    });
}

} // namespace raikiri::venues::gate
