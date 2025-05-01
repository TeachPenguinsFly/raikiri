#include "wire_detail.hpp"

namespace raikiri::venues::mexc {

namespace {

using detail::guarded;
using detail::session;

} // namespace

std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body) {
    return guarded([&]() -> std::expected<RestEnvelope, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        bool success = false;
        if (const auto ec = root->find_field_unordered("success").get_bool().get(success);
            ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        const auto code = detail::opt_i64(*root, "code", -1);
        if (!code.has_value()) {
            return std::unexpected(code.error());
        }
        if (*code < 0) {
            return std::unexpected(WireError::MissingField);
        }
        // Errors name the field `message`; successful bodies omit it entirely.
        const auto msg = detail::opt_scalar(*root, "message");
        if (!msg.has_value()) {
            return std::unexpected(msg.error());
        }
        return RestEnvelope{.success = success, .code = *code, .msg = *msg};
    });
}

std::expected<std::span<const WireInstrument>, WireError>
parse_instruments(std::string_view body, InstrumentBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireInstrument>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = detail::req_array(*root, "data");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_instruments) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WireInstrument out{};
            const auto symbol = detail::req_scalar(obj, "symbol");
            if (!symbol.has_value()) {
                return std::unexpected(symbol.error());
            }
            out.symbol = *symbol;
            const auto contract_size = detail::req_scalar(obj, "contractSize");
            const auto min_vol = detail::req_scalar(obj, "minVol");
            if (!contract_size.has_value() || !min_vol.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.contract_size = *contract_size;
            out.min_vol = *min_vol;
            const auto price_scale = detail::opt_i64(obj, "priceScale", -1);
            const auto vol_scale = detail::opt_i64(obj, "volScale", -1);
            const auto state = detail::opt_i64(obj, "state", -1);
            const auto api_allowed = detail::opt_bool(obj, "apiAllowed");
            if (!price_scale.has_value() || !vol_scale.has_value() || !state.has_value() ||
                !api_allowed.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.price_scale = *price_scale;
            out.vol_scale = *vol_scale;
            out.state = *state;
            out.api_allowed = *api_allowed;
            storage.instruments[count] = out;
            ++count;
        }
        return std::span<const WireInstrument>{storage.instruments.data(), count};
    });
}

std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body) {
    return guarded([&]() -> std::expected<std::uint64_t, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto ts = detail::opt_ms(*root, "data");
        if (!ts.has_value()) {
            return std::unexpected(ts.error());
        }
        if (*ts == 0) {
            return std::unexpected(WireError::MissingField);
        }
        return *ts;
    });
}

std::expected<WireFundingRate, WireError> parse_funding_rate(std::string_view body) {
    return guarded([&]() -> std::expected<WireFundingRate, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = detail::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireFundingRate out{};
        const auto symbol = detail::req_scalar(*data, "symbol");
        const auto rate = detail::req_scalar(*data, "fundingRate");
        if (!symbol.has_value() || !rate.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.symbol = *symbol;
        out.funding_rate = *rate;
        const auto cycle = detail::opt_i64(*data, "collectCycle", 0);
        const auto next = detail::opt_ms(*data, "nextSettleTime");
        const auto ts = detail::opt_ms(*data, "timestamp");
        const auto fair = detail::opt_scalar(*data, "fairPrice");
        if (!cycle.has_value() || !next.has_value() || !ts.has_value() || !fair.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.collect_cycle_h = *cycle;
        out.next_settle_ms = *next;
        out.ts_ms = *ts;
        out.fair_price = *fair;
        return out;
    });
}

} // namespace raikiri::venues::mexc
