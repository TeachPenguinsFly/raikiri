#pragma once

// state feeds are sampled from seqlock tables and dropped while the socket is busy
// events are sequenced from the broadcast ring; an overrun sends a gap frame

#include <raikiri/admin/registry.hpp>
#include <raikiri/admin/types.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/shm/records.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::admin {

enum class Op : std::uint8_t { Subscribe, Unsubscribe };

// the first six feeds are sampled state; the last three are sequenced
enum class Feed : std::uint8_t {
    Quotes,
    Book,
    Greeks,
    Funding,
    Ticker,
    Positions,
    Health,
    Orders,
    Fills,
    Recon,
};

[[nodiscard]] bool sequenced(Feed feed) noexcept;

// `millihertz` is the requested rate, not the negotiated one
struct ClientMessage {
    Op op{Op::Subscribe};
    std::string id;
    std::vector<InstrumentId> quotes;
    std::optional<InstrumentId> book;
    std::optional<std::size_t> levels;
    std::vector<InstrumentId> greeks;
    std::vector<InstrumentId> funding;
    std::vector<InstrumentId> ticker;
    std::vector<Feed> events;
    std::optional<std::int64_t> millihertz;
};

// a malformed message yields an error frame, not a dropped connection
[[nodiscard]] std::expected<ClientMessage, Problem> parse_client_message(std::string_view text);

// cadence in millihertz: 30 Hz is 30000
inline constexpr std::int64_t k_max_millihertz = 30'000; // the server's own sampling rate
inline constexpr std::int64_t k_min_millihertz = 100;

[[nodiscard]] std::int64_t default_millihertz(Feed feed, std::size_t instruments) noexcept;

// requests above k_max_millihertz are clamped; sequenced feeds have no cadence
[[nodiscard]] std::int64_t
negotiate_millihertz(Feed feed, std::size_t instruments, std::optional<std::int64_t> asked) noexcept;

[[nodiscard]] std::uint64_t period_ns(std::int64_t millihertz) noexcept;

// seqlock versions last sent per slot; recorded only after the frame reached the socket
class ChangeGate {
public:
    ChangeGate() = default;

    explicit ChangeGate(std::size_t slots) : versions_(slots, 0) {}

    // version 0 is a never-written cell
    [[nodiscard]] bool moved(std::size_t slot, std::uint32_t version) const noexcept;
    void accept(std::size_t slot, std::uint32_t version) noexcept;

private:
    std::vector<std::uint32_t> versions_;
};

// `listed` points into a segment mapping; re-resolve when Registry::epoch() moves
struct Target {
    const Gateway* gateway{nullptr};
    const raikiri::Instrument* listed{nullptr};
    InstrumentId id{};
    std::size_t cell{0};
};

// unresolved ids are appended to `unknown`
[[nodiscard]] std::vector<Target>
resolve_targets(const Registry& registry, std::span<const InstrumentId> ids, std::vector<std::string>& unknown);

// nullopt until both sides are set
[[nodiscard]] std::optional<Quote> quote_of(const Bbo& bbo, const InstrumentBase& base);

// `levels` caps each side, clamped to what the snapshot holds
[[nodiscard]] Book book_of(const BookSnapshot& snapshot, const InstrumentBase& base, std::size_t levels);

inline constexpr std::size_t k_default_levels = 25;
[[nodiscard]] Greeks greeks_of(const OptionGreeks& greeks);
[[nodiscard]] Funding funding_of(const FundingUpdate& funding, const InstrumentBase& base);

// three missed 30 s reconcile pulls
inline constexpr std::uint64_t k_position_stale_ns = 90ULL * 1'000'000'000ULL;

// a cell older than the staleness window gives a null `venue_qty`; null is not flat
[[nodiscard]] Position position_of(const shm::PositionCell& cell, const InstrumentBase& base, UnixNanos now);

[[nodiscard]] std::string ack_frame(std::string_view id, std::optional<std::int64_t> millihertz);
[[nodiscard]] std::string quotes_frame(std::string_view id, UnixNanos ts, std::span<const Quote> quotes);
[[nodiscard]] std::string book_frame(std::string_view id, UnixNanos ts, const Book& book);
[[nodiscard]] std::string greeks_frame(std::string_view id, UnixNanos ts, std::span<const Greeks> greeks);
[[nodiscard]] std::string funding_frame(std::string_view id, UnixNanos ts, std::span<const Funding> funding);
[[nodiscard]] std::string positions_frame(std::string_view id, UnixNanos ts, std::span<const Position> positions);
[[nodiscard]] std::string health_frame(std::string_view id, UnixNanos ts, const Health& health);

// a fill is the Filled order event rendered as a Fill
[[nodiscard]] std::string
order_event_frame(std::string_view id, std::uint64_t seq, const raikiri::OrderEvent& event, const InstrumentBase& base);
[[nodiscard]] std::string
fill_frame(std::string_view id, std::uint64_t seq, const raikiri::OrderEvent& event, const InstrumentBase& base);

[[nodiscard]] std::string gap_frame(std::string_view id, std::uint64_t missed);
[[nodiscard]] std::string problem_frame(std::string_view id, const Problem& problem);

// single-threaded and poll-driven; binds its own port
class Stream {
public:
    Stream(const Registry& registry, const Clock& clock, std::string address, std::uint16_t port);
    Stream(net::IoContext& io, const Registry& registry, const Clock& clock, std::string address, std::uint16_t port);
    ~Stream();
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    [[nodiscard]] bool start();

    // samples what is due and runs the reactor; returns work done
    std::size_t poll() noexcept;

    // when the next poll() has something to sample
    [[nodiscard]] MonoNanos next_due() const noexcept;

    void stop() noexcept;

    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] std::size_t clients() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace raikiri::admin
