#pragma once

// fees and maintenance-margin tiers per account and instrument; the gateway holds the key and the rate budget, so
// admind asks it over the segment and keeps the answer for an hour

#include <raikiri/admin/margin.hpp>
#include <raikiri/admin/registry.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/terms.hpp>
#include <raikiri/http/server.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace raikiri::admin {

// a whole answer, its parts put back together; a failed one carries the reason
struct TermsAnswer {
    TermsOutcome outcome{TermsOutcome::Done};
    ErrorClass cls{};
    std::string reason;
    TradingTerms terms;
};

// asked without waiting, like MarginGateway: a reply is collected from the loop's pump
class TermsGateway {
public:
    TermsGateway() = default;
    TermsGateway(const TermsGateway&) = delete;
    TermsGateway& operator=(const TermsGateway&) = delete;
    virtual ~TermsGateway() = default;

    [[nodiscard]] virtual std::optional<MarginTarget> target(InstrumentId instrument) const = 0;
    // the instrument's mid now, zero where it has none; prices tiers a venue counts in base units
    [[nodiscard]] virtual double price(InstrumentId instrument) const = 0;
    [[nodiscard]] virtual std::expected<void, MarginFault> send(TermsRequest& request) = 0;
    // the answer once its last part lands, nullopt before, NoAnswer once its wait has passed
    [[nodiscard]] virtual std::expected<std::optional<TermsAnswer>, MarginFault> collect(std::uint64_t id) = 0;
};

class ShmTermsGateway final : public TermsGateway {
public:
    ShmTermsGateway(const Registry& registry,
                    std::function<MonoNanos()> now,
                    std::chrono::milliseconds wait = std::chrono::milliseconds{12'000});
    ~ShmTermsGateway() override;

    [[nodiscard]] std::optional<MarginTarget> target(InstrumentId instrument) const override;
    [[nodiscard]] double price(InstrumentId instrument) const override;
    [[nodiscard]] std::expected<void, MarginFault> send(TermsRequest& request) override;
    [[nodiscard]] std::expected<std::optional<TermsAnswer>, MarginFault> collect(std::uint64_t id) override;

private:
    struct Waiting;

    const Registry* registry_;
    std::function<MonoNanos()> now_;
    std::chrono::milliseconds wait_;
    std::uint64_t seq_{0};
    std::vector<std::unique_ptr<Waiting>> waiting_;
};

struct TermsView {
    std::string account;
    InstrumentId instrument{};
    TradingTerms terms; // tiers in quote notional, whatever the venue counted them in
    UnixNanos as_of{};
};

using TermsDone = std::function<void(std::expected<TermsView, MarginError>)>;

inline constexpr std::uint64_t k_terms_ttl_ns = 3'600ULL * 1'000'000'000ULL;
inline constexpr std::size_t k_terms_cache_rows = 2'048;

// `done` runs from pump once the gateway answers, or at once from the cache or when the request never leaves
class TermsDesk {
public:
    TermsDesk(TermsGateway& gateway, std::function<UnixNanos()> wall);

    void read(std::string_view account, InstrumentId instrument, TermsDone done);

    void pump();

    [[nodiscard]] bool pending() const noexcept { return !waiting_.empty(); }

private:
    struct Waiting {
        std::uint64_t id{0};
        std::string account;
        InstrumentId instrument{};
        // everyone who asked for the same instrument while the gateway was out
        std::vector<TermsDone> done;
    };

    struct Kept {
        TradingTerms terms;
        UnixNanos as_of{};
    };

    [[nodiscard]] TermsView view(std::string_view account, InstrumentId instrument, const Kept& kept) const;
    void finish(Waiting& waiting, const std::expected<TermsAnswer, MarginFault>& answer);

    TermsGateway* gateway_;
    std::function<UnixNanos()> wall_;
    std::vector<Waiting> waiting_;
    // an instrument belongs to one account, so its id keys the pair
    std::unordered_map<std::uint64_t, Kept> kept_;
};

// GET /v1/accounts/{account}/terms; false for any other path, which leaves `respond` unused
class TermsRoutes {
public:
    explicit TermsRoutes(TermsDesk& desk) : desk_{&desk} {}

    [[nodiscard]] bool operator()(const http::Request& req, const MarginRespond& respond) const;

private:
    TermsDesk* desk_;
};

// shortest fixed notation that reads back the same: 0.0002, never 2e-04
[[nodiscard]] std::string decimal_text(double value);

} // namespace raikiri::admin
