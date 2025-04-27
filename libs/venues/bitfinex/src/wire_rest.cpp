#include "wire_detail.hpp"

namespace raikiri::venues::bitfinex {

namespace {

using detail::guarded;
using detail::session;

constexpr std::string_view k_error_tag = "error";

// Matched against the raw body, quotes included, so the sniff below never has
// to index into a buffer whose length it has not already checked.
constexpr std::string_view k_quoted_error_tag = R"("error")";

// The /conf endpoints take a list of keys and answer one result per key, so a
// single-key request still arrives wrapped one level deeper than its payload.
constexpr std::size_t k_conf_results = 1;

} // namespace

bool is_rest_error(std::string_view body) noexcept {
    auto text = detail::trim(body);
    if (text.empty() || text.front() != '[') {
        return false;
    }
    text.remove_prefix(1);
    // An error is the only Bitfinex REST answer whose first element is a
    // string, which is the whole of the discrimination available: there is no
    // envelope and no status field to read.
    return detail::trim(text).starts_with(k_quoted_error_tag);
}

std::expected<RestError, WireError> parse_rest_error(std::string_view body) {
    return guarded([&]() -> std::expected<RestError, WireError> {
        auto root = session().open_array(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto row = detail::read_row<k_rest_error_arity>(*root);
        if (!row.has_value()) {
            return std::unexpected(row.error());
        }
        if (row->at<k_rest_error_tag>() != k_error_tag) {
            return std::unexpected(WireError::WrongType);
        }
        std::int64_t code = 0;
        if (!detail::token_to_i64(row->at<k_rest_error_code>(), code)) {
            return std::unexpected(WireError::WrongType);
        }
        return RestError{.code = code, .msg = row->at<k_rest_error_msg>()};
    });
}

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
            detail::ondemand::array pairs;
            if (const auto ec = result.get_array().get(pairs); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            for (auto entry : pairs) {
                if (count == k_max_pairs) {
                    return std::unexpected(WireError::TooManyEntries);
                }
                detail::ondemand::array pair;
                if (const auto ec = entry.get_array().get(pair); ec != simdjson::SUCCESS) {
                    return std::unexpected(detail::map_error(ec));
                }
                WirePairInfo out{};
                std::size_t index = 0;
                for (auto field : pair) {
                    detail::ondemand::value value;
                    if (const auto ec = field.get(value); ec != simdjson::SUCCESS) {
                        return std::unexpected(detail::map_error(ec));
                    }
                    if (index == 0) {
                        const auto token = detail::token_of(value);
                        if (!token.has_value()) {
                            return std::unexpected(token.error());
                        }
                        out.pair = *token;
                        ++index;
                        continue;
                    }
                    // The row's own arity differs by market — 12 on spot, 10
                    // on futures — so only the prefix the mapper reads is
                    // required, and the tail is left unwalked.
                    const auto row = detail::read_row<k_pair_row_arity>(value);
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

std::expected<std::int64_t, WireError> parse_platform_status(std::string_view body) {
    return guarded([&]() -> std::expected<std::int64_t, WireError> {
        auto root = session().open_array(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto row = detail::read_row<1>(*root);
        if (!row.has_value()) {
            return std::unexpected(row.error());
        }
        std::int64_t status = 0;
        if (!detail::token_to_i64(row->at<0>(), status)) {
            return std::unexpected(WireError::WrongType);
        }
        return status;
    });
}

} // namespace raikiri::venues::bitfinex
