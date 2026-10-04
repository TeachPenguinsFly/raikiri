#pragma once

// nanosecond stamps leave as digit strings

#include <raikiri/introspect/stats.hpp>

#include <glaze/glaze.hpp>

template <> struct glz::meta<raikiri::introspect::ProcessStats> {
    using T = raikiri::introspect::ProcessStats;
    static constexpr auto value = object("pid",
                                         &T::pid,
                                         "generation",
                                         glz::quoted_num<&T::generation>,
                                         "started_ns",
                                         glz::quoted_num<&T::started_ns>,
                                         "uptime_s",
                                         &T::uptime_s,
                                         "build",
                                         &T::build);
};

template <> struct glz::meta<raikiri::introspect::SocketStats> {
    using T = raikiri::introspect::SocketStats;
    static constexpr auto value = object("host",
                                         &T::host,
                                         "sockets",
                                         &T::sockets,
                                         "open",
                                         &T::open,
                                         "keys",
                                         &T::keys,
                                         "min_keys",
                                         &T::min_keys,
                                         "avg_keys",
                                         &T::avg_keys,
                                         "max_keys",
                                         &T::max_keys,
                                         "pending_frames",
                                         &T::pending_frames,
                                         "reopens",
                                         &T::reopens,
                                         "last_close_reason",
                                         &T::last_close_reason,
                                         "last_close_ns",
                                         glz::quoted_num<&T::last_close_ns>,
                                         "backoffs",
                                         &T::backoffs,
                                         "refusals",
                                         &T::refusals,
                                         "backoff_ms",
                                         &T::backoff_ms);
};

template <> struct glz::meta<raikiri::introspect::ClockStats> {
    using T = raikiri::introspect::ClockStats;
    static constexpr auto value = object("venue",
                                         &T::venue,
                                         "method",
                                         &T::method,
                                         "endpoint",
                                         &T::endpoint,
                                         "offset_ns",
                                         &T::offset_ns,
                                         "uncertainty_ns",
                                         &T::uncertainty_ns,
                                         "rtt_ns",
                                         &T::rtt_ns,
                                         "sampled_ns",
                                         glz::quoted_num<&T::sampled_ns>,
                                         "samples",
                                         &T::samples,
                                         "drift_ns",
                                         &T::drift_ns,
                                         "drift_over_ns",
                                         &T::drift_over_ns);
};

template <> struct glz::meta<raikiri::introspect::GatewayStats> {
    using T = raikiri::introspect::GatewayStats;
    static constexpr auto value = object("version",
                                         &T::version,
                                         "taken_ns",
                                         glz::quoted_num<&T::taken_ns>,
                                         "account",
                                         &T::account,
                                         "venue",
                                         &T::venue,
                                         "market",
                                         &T::market,
                                         "feed",
                                         &T::feed,
                                         "channels",
                                         &T::channels,
                                         "process",
                                         &T::process,
                                         "catalogue",
                                         &T::catalogue,
                                         "subscriptions",
                                         &T::subscriptions,
                                         "sockets",
                                         &T::sockets,
                                         "limits",
                                         &T::limits,
                                         "coverage",
                                         &T::coverage,
                                         "rates",
                                         &T::rates,
                                         "clock",
                                         &T::clock,
                                         "latency",
                                         &T::latency);
};
