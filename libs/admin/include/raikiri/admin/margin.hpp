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
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

// asked without waiting: admind's one loop serves the http routes and the stream, so a reply is collected later
class MarginGateway {
public:
    MarginGateway() = default;
    MarginGateway(const MarginGateway&) = delete;
    MarginGateway& operator=(const MarginGateway&) = delete;
    virtual ~MarginGateway() = default;

    [[nodiscard]] virtual std::optional<MarginTarget> target(InstrumentId instrument) const = 0;
    // pushes the request and fills in its id
    [[nodiscard]] virtual std::expected<void, MarginFault> send(MarginRequest& request) = 0;
    // the reply once it lands, nullopt before, NoAnswer once its wait has passed; either answer forgets the id
    [[nodiscard]] virtual std::expected<std::optional<MarginReply>, MarginFault> collect(std::uint64_t id) = 0;
};

class ShmMarginGateway final : public MarginGateway {
public:
    ShmMarginGateway(const Registry& registry,
                     std::function<MonoNanos()> now,
                     std::chrono::milliseconds wait = std::chrono::milliseconds{8'000});
    ~ShmMarginGateway() override;

    [[nodiscard]] std::optional<MarginTarget> target(InstrumentId instrument) const override;
    [[nodiscard]] std::expected<void, MarginFault> send(MarginRequest& request) override;
    [[nodiscard]] std::expected<std::optional<MarginReply>, MarginFault> collect(std::uint64_t id) override;

private:
    struct Waiting;

    const Registry* registry_;
    std::function<MonoNanos()> now_;
    std::chrono::milliseconds wait_;
    std::uint64_t seq_{0};
    std::vector<std::unique_ptr<Waiting>> waiting_;
};

struct MarginChange {
    std::optional<std::uint32_t> leverage{};
    std::optional<MarginMode> mode{};
};

struct MarginView {
    std::string account;
    InstrumentId instrument{};
    MarginState state{};
    MarginScope scope{MarginScope::Unknown};
};

struct MarginError {
    int status{500};
    std::string title;
    std::string detail;
};

// no venue lists more, so anything above is a typing slip
inline constexpr std::uint32_t k_max_leverage_asked = 1'000;

using MarginDone = std::function<void(std::expected<MarginView, MarginError>)>;

// `done` runs from pump once the gateway answers, or at once when the request never leaves admind
class MarginDesk {
public:
    // a null journal refuses every change: one that leaves no record is not made
    MarginDesk(MarginGateway& gateway, audit::Sink* journal, std::function<UnixNanos()> wall);

    void read(std::string_view account, InstrumentId instrument, MarginDone done);

    void set(std::string_view account,
             InstrumentId instrument,
             const MarginChange& change,
             std::string_view actor,
             MarginDone done);

    // collects what has landed or timed out; the caller's loop runs it every few tens of milliseconds while pending
    void pump();

    [[nodiscard]] bool pending() const noexcept { return !waiting_.empty(); }

private:
    struct Waiting {
        std::uint64_t id{0};
        MarginTarget target;
        InstrumentId instrument{};
        std::optional<MarginChange> change; // empty for a read
        std::string actor;
        MarginDone done;
    };

    [[nodiscard]] std::expected<MarginTarget, MarginError> resolve(std::string_view account,
                                                                   InstrumentId instrument) const;
    void finish(Waiting& waiting, const std::expected<MarginReply, MarginFault>& reply);

    MarginGateway* gateway_;
    audit::Sink* journal_;
    std::function<UnixNanos()> wall_;
    std::vector<Waiting> waiting_;
};

using MarginRespond = std::function<void(http::Response)>;

// GET and PUT /v1/accounts/{account}/leverage; false for any other path, which leaves `respond` unused
class MarginRoutes {
public:
    explicit MarginRoutes(MarginDesk& desk) : desk_{&desk} {}

    [[nodiscard]] bool operator()(const http::Request& req, const MarginRespond& respond) const;

private:
    MarginDesk* desk_;
};

} // namespace raikiri::admin
