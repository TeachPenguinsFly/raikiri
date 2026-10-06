#pragma once

#include <raikiri/domain/identifiers.hpp>

#include <cstddef>
#include <string_view>

namespace raikiri::venues::upbit {

// what differs between venues serving the upbit v1 api
struct Flavour {
    VenueId venue{VenueId::Invalid};
    std::string_view name{};
    std::string_view rest_host{};
    std::string_view ws_host{};
    std::string_view ws_path{};
    // one request answering every market; empty asks by lists of `ticker_chunk` markets
    std::string_view ticker_all{};
    std::size_t ticker_chunk{100};
    // false where the rest ticker's timestamps run nine hours ahead
    bool ticker_stamped{true};
    // units asked for on a quote; a whole book per change is more than a socket from korea carries
    std::string_view quote_units{".1"};
};

inline constexpr Flavour k_upbit{.venue = VenueId::Upbit,
                                 .name = "upbit",
                                 .rest_host = "api.upbit.com",
                                 .ws_host = "api.upbit.com",
                                 .ws_path = "/websocket/v1",
                                 .ticker_all = "/v1/ticker/all?quote_currencies=KRW,BTC,USDT",
                                 .ticker_chunk = 100,
                                 .ticker_stamped = true,
                                 .quote_units = ".1"};

} // namespace raikiri::venues::upbit
