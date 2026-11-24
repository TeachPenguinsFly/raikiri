#pragma once

// leverage and margin mode per account and instrument; the gateway holds the keys and the rate budget, so admind
// asks it over the segment and journals what changed

#include <raikiri/admin/registry.hpp>
#include <raikiri/audit/log.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/margin.hpp>
#include <raikiri/http/server.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace raikiri::admin {

struct MarginTarget {
    std::string account;
    std::string symbol;
};

enum class MarginFault : std::uint8_t {
    GatewayGone, // listed, but its gateway is not answering
    NotServed,   // the gateway predates the call, or runs no account poller for want of keys
    RingHeld,    // another admind produces this gateway's requests
    RingFull,
    NoAnswer, // nothing came back in time; the change may still land
};

class MarginGateway {
public:
    MarginGateway() = default;
    MarginGateway(const MarginGateway&) = delete;
    MarginGateway& operator=(const MarginGateway&) = delete;
    virtual ~MarginGateway() = default;

    [[nodiscard]] virtual std::optional<MarginTarget> target(InstrumentId instrument) const = 0;
    // fills in the request's id
    [[nodiscard]] virtual std::expected<MarginReply, MarginFault> ask(MarginRequest request) = 0;
};

// blocks the caller until the poller answers, as the upstream fetches do; `wait` bounds it
class ShmMarginGateway final : public MarginGateway {
public:
    ShmMarginGateway(const Registry& registry,
                     std::function<MonoNanos()> now,
                     std::chrono::milliseconds wait = std::chrono::milliseconds{8'000});

    [[nodiscard]] std::optional<MarginTarget> target(InstrumentId instrument) const override;
    [[nodiscard]] std::expected<MarginReply, MarginFault> ask(MarginRequest request) override;

private:
    const Registry* registry_;
    std::function<MonoNanos()> now_;
    std::chrono::milliseconds wait_;
    std::uint64_t seq_{0};
};

struct MarginChange {
    std::optional<std::uint32_t> leverage{};
    std::optional<MarginMode> mode{};
};

struct MarginView {
    std::string account;
    InstrumentId instrument{};
    MarginState state{};
};

struct MarginError {
    int status{500};
    std::string title;
    std::string detail;
};

// no venue lists more, so anything above is a typing slip
inline constexpr std::uint32_t k_max_leverage_asked = 1'000;

class MarginDesk {
public:
    // a null journal refuses every change: one that leaves no record is not made
    MarginDesk(MarginGateway& gateway, audit::Sink* journal, std::function<UnixNanos()> wall);

    [[nodiscard]] std::expected<MarginView, MarginError> read(std::string_view account, InstrumentId instrument);

    [[nodiscard]] std::expected<MarginView, MarginError>
    set(std::string_view account, InstrumentId instrument, const MarginChange& change, std::string_view actor);

private:
    [[nodiscard]] std::expected<MarginTarget, MarginError> resolve(std::string_view account,
                                                                   InstrumentId instrument) const;

    MarginGateway* gateway_;
    audit::Sink* journal_;
    std::function<UnixNanos()> wall_;
};

// GET and PUT /v1/accounts/{account}/leverage; nullopt for any other path
class MarginRoutes {
public:
    explicit MarginRoutes(MarginDesk& desk) : desk_{&desk} {}

    [[nodiscard]] std::optional<http::Response> operator()(const http::Request& req) const;

private:
    MarginDesk* desk_;
};

} // namespace raikiri::admin
