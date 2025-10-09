#include <raikiri/venues/bitmex/catalogue.hpp>
#include <raikiri/venues/bitmex/envelope.hpp>
#include <raikiri/wire/json.hpp>

#include <utility>

namespace raikiri::venues::bitmex {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

} // namespace

std::expected<std::span<const WireInstrument>, WireError>
parse_instruments(std::string_view body, InstrumentBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireInstrument>, WireError> {
        auto array = session().open_array(body);
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
            const auto symbol = detail::req_str(obj, "symbol");
            const auto state = detail::req_str(obj, "state");
            const auto typ = detail::req_str(obj, "typ");
            if (!symbol.has_value() || !state.has_value() || !typ.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.symbol = *symbol;
            out.state = *state;
            out.typ = *typ;
            for (const auto& [key, field] :
                 {std::pair{"tickSize", &out.tick_size},
                  std::pair{"lotSize", &out.lot_size},
                  std::pair{"multiplier", &out.multiplier},
                  std::pair{"underlyingToSettleMultiplier", &out.underlying_to_settle},
                  std::pair{"quoteToSettleMultiplier", &out.quote_to_settle},
                  std::pair{"underlyingToPositionMultiplier", &out.underlying_to_position},
                  std::pair{"makerFee", &out.maker_fee},
                  std::pair{"takerFee", &out.taker_fee}}) {
                const auto value = opt_num(obj, key);
                if (!value.has_value()) {
                    return std::unexpected(value.error());
                }
                *field = *value;
            }
            const auto expiry = detail::opt_str(obj, "expiry");
            const auto interval = detail::opt_str(obj, "fundingInterval");
            const auto inverse = detail::opt_bool(obj, "isInverse");
            const auto quanto = detail::opt_bool(obj, "isQuanto");
            if (!expiry.has_value() || !interval.has_value() || !inverse.has_value() ||
                !quanto.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.expiry = *expiry;
            out.funding_interval = *interval;
            out.inverse = *inverse;
            out.quanto = *quanto;
            storage.instruments[count] = out;
            ++count;
        }
        return std::span<const WireInstrument>{storage.instruments.data(), count};
    });
}

} // namespace raikiri::venues::bitmex
