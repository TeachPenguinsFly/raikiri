#pragma once

#include <raikiri/admin/desk.hpp>
#include <raikiri/admin/history.hpp>
#include <raikiri/admin/registry.hpp>
#include <raikiri/admin/warm.hpp>
#include <raikiri/http/server.hpp>

#include <map>
#include <string>
#include <vector>

namespace raikiri::admin {

// the market-data channels each account's gateway has carried, by account; one read per request
using LiveChannels = std::function<std::map<std::string, std::vector<std::string>>()>;

// `now` is monotonic and `wall` is wall clock; a null `history` or `desk` answers 503 on its paths
// `upstreams` maps a name to a loopback port; /v1/x/<name>/... is forwarded verbatim
[[nodiscard]] http::Handler make_router(const Registry& registry,
                                        const History* history,
                                        ManualDesk* desk,
                                        std::function<MonoNanos()> now,
                                        std::function<UnixNanos()> wall,
                                        std::map<std::string, std::uint16_t> upstreams = {},
                                        books::Registry* books = nullptr,
                                        const MarketsCache* markets = nullptr,
                                        LiveChannels live_channels = {});

// rfc 9457 problem response
[[nodiscard]] http::Response problem(int status, std::string_view title, std::string_view detail = {});

} // namespace raikiri::admin
