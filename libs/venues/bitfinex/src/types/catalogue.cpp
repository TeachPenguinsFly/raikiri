#include <raikiri/venues/bitfinex/catalogue.hpp>
#include <raikiri/venues/bitfinex/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::bitfinex {

namespace {

using wire::guarded;
using wire::session;

// /conf answers one result per key, so even one key is wrapped
constexpr std::size_t k_conf_results = 1;

} // namespace

std::expected<std::span<const WirePairInfo>, WireError> parse_pair_info(std::string_view body,
                                                                        PairBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WirePairInfo>, WireError> {
        auto root = session().open_array(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        std::size_t results = 0;
        std::size_t count = 0;
        for (auto result : *root) {
            if (results == k_conf_results) {
                break;
            }
            ++results;
            wire::ondemand::array pairs;
            if (const auto ec = result.get_array().get(pairs); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            for (auto entry : pairs) {
                if (count == k_max_pairs) {
                    return std::unexpected(WireError::TooManyEntries);
                }
                wire::ondemand::array pair;
                if (const auto ec = entry.get_array().get(pair); ec != simdjson::SUCCESS) {
                    return std::unexpected(wire::map_error(ec));
                }
                WirePairInfo out{};
                std::size_t index = 0;
                for (auto field : pair) {
                    wire::ondemand::value value;
                    if (const auto ec = field.get(value); ec != simdjson::SUCCESS) {
                        return std::unexpected(wire::map_error(ec));
                    }
                    if (index == 0) {
                        const auto token = token_of(value);
                        if (!token.has_value()) {
                            return std::unexpected(token.error());
                        }
                        out.pair = *token;
                        ++index;
                        continue;
                    }
                    const auto row = read_row<k_pair_row_arity>(value);
                    if (!row.has_value()) {
                        return std::unexpected(row.error());
                    }
                    out.min_order_size = row->at<k_pair_min_order_size>();
                    out.max_order_size = row->at<k_pair_max_order_size>();
                    ++index;
                    break;
                }
                if (index < 2 || out.pair.empty()) {
                    return std::unexpected(WireError::ShortArray);
                }
                storage.pairs[count] = out;
                ++count;
            }
        }
        if (results == 0) {
            return std::unexpected(WireError::MissingField);
        }
        return std::span<const WirePairInfo>{storage.pairs.data(), count};
    });
}

} // namespace raikiri::venues::bitfinex
