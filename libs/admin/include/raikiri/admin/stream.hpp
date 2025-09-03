#pragma once

// The live half of admind: one WebSocket carrying everything that moves.
//
// See the /v1/stream description in schema/admin-api.yaml for the protocol.
//
// TWO KINDS OF DATA, AND THE DIFFERENCE IS THE DESIGN.
//
// State — quotes, book, greeks, funding, positions, health — is SAMPLED from
// the gateway's seqlock tables at a cadence: the version is read first and the
// payload copied only when it moved, and a frame the client's socket cannot
// take right now is DROPPED and re-sampled next tick. There is no queue
// between a venue and a browser, so a slow client gets fewer frames and never
// accumulating lag — a quote it did not receive had already been replaced by
// one it did.
//
// Events — order events, fills — are sequenced and never coalesced, because
// every fill is money. The gateway's broadcast ring is their queue, and an
// overrun is said out loud in a gap frame rather than leaving a client to
// carry a position it never heard about.
//
// Everything below the Stream class is a pure function of its arguments, which
// is what lets the protocol be tested without a socket or a running gateway.

#include <raikiri/admin/registry.hpp>
#include <raikiri/admin/types.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>
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

// ------------------------------------------------------- subscriptions ----

enum class Op : std::uint8_t { Subscribe, Unsubscribe };

// One channel of one subscription. The first six are state and are sampled;
// the last three are sequenced and are drained.
enum class Feed : std::uint8_t {
    Quotes,
    Book,
    Greeks,
    Funding,
    Positions,
    Health,
    Orders,
    Fills,
    Recon,
};

[[nodiscard]] bool sequenced(Feed feed) noexcept;

// A client message whose ids have already been read as digits. `millihertz`
// is the ask, not the answer.
struct ClientMessage {
    Op op{Op::Subscribe};
    std::string id;
    std::vector<InstrumentId> quotes;
    std::optional<InstrumentId> book;
    // How deep a book to send. A visualiser draws twenty-odd levels; the
    // snapshot holds a hundred, and a hundred levels times several venues times
    // a frame a second is the bandwidth nobody was looking at.
    std::optional<std::size_t> levels;
    std::vector<InstrumentId> greeks;
    std::vector<InstrumentId> funding;
    std::vector<Feed> events;
    std::optional<std::int64_t> millihertz;
};

// A malformed message is one bad frame, never a dropped connection: a terminal
// that mistypes a subscription should hear about it and keep its stream.
[[nodiscard]] std::expected<ClientMessage, Problem> parse_client_message(std::string_view text);

// ------------------------------------------------------------ cadence ----

// Cadence is carried in MILLIHERTZ so no decision here needs a double: 30 Hz
// is 30000 and half a hertz is 500. The contract's `hz` is a JSON number and
// is rendered from this at the edge, where it is small enough to be one
// honestly.
inline constexpr std::int64_t k_max_millihertz = 30'000; // the server's own sampling rate
inline constexpr std::int64_t k_min_millihertz = 100;

// What a feed runs at when the client did not ask. A focused view — one
// instrument's book or quote — earns 30 Hz; a watchlist of them does not, and
// greeks, positions and health move far slower than any display refreshes.
[[nodiscard]] std::int64_t default_millihertz(Feed feed, std::size_t instruments) noexcept;

// The ask, answered. Above the server's own sampling rate it is answered with
// that rate rather than refused, because a client asking for 200 Hz is asking
// for frames the tables do not hold. A sequenced feed has no cadence at all.
[[nodiscard]] std::int64_t negotiate_millihertz(Feed feed,
                                                std::size_t instruments,
                                                std::optional<std::int64_t> asked) noexcept;

[[nodiscard]] std::uint64_t period_ns(std::int64_t millihertz) noexcept;

// The seqlock versions a subscription last SENT, one slot per instrument it
// follows.
//
// The version is checked before the payload is copied — that is what keeps an
// unchanged cell off the wire — and recorded only once the frame has reached
// the socket. A frame dropped because the client was busy must be re-sampled
// on the next tick, so remembering it as sent would hide the value until the
// venue happened to move it again.
class ChangeGate {
public:
    ChangeGate() = default;

    explicit ChangeGate(std::size_t slots) : versions_(slots, 0) {}

    // Version 0 is a cell that was never written, which is absence, not a value.
    [[nodiscard]] bool moved(std::size_t slot, std::uint32_t version) const noexcept;
    void accept(std::size_t slot, std::uint32_t version) noexcept;

private:
    std::vector<std::uint32_t> versions_;
};

// --------------------------------------------------- records into rows ----

// A cell with a side missing is not a quote yet: the contract requires all
// four numbers, and an undefined one renders empty, which is not a decimal.
[[nodiscard]] std::optional<Quote> quote_of(const Bbo& bbo, const InstrumentBase& base);

// `levels` caps each side. Clamped to what the snapshot actually holds, so a
// client asking for more than the venue sent gets what the venue sent rather
// than a ladder padded with the previous snapshot's tail.
[[nodiscard]] Book
book_of(const BookSnapshot& snapshot, const InstrumentBase& base, std::size_t levels);

// What a book subscription sends when the client did not say. Twenty-five is
// what a depth visualiser draws; asking for the hundred the snapshot can hold
// is asking for four times the bytes to render the same picture.
inline constexpr std::size_t k_default_levels = 25;
// No instrument needed: every field is already a ratio or a quote-currency
// price, and the option's own tick is the premium's step.
[[nodiscard]] Greeks greeks_of(const OptionGreeks& greeks);
[[nodiscard]] Funding funding_of(const FundingUpdate& funding, const InstrumentBase& base);

// Three missed pulls, gatewayd reconciling every 30 s: one late round is not a
// venue going quiet.
inline constexpr std::uint64_t k_position_stale_ns = 90ULL * 1'000'000'000ULL;

// A venue's positions age out rather than decaying into flatness: a pull that
// failed writes nothing, so a cell older than `now` minus the staleness window
// is a venue that has not answered and its `venue_qty` is null. NULL IS NOT
// FLAT.
[[nodiscard]] Position
position_of(const shm::PositionCell& cell, const InstrumentBase& base, UnixNanos now);

// ------------------------------------------------------------- frames ----

[[nodiscard]] std::string ack_frame(std::string_view id, std::optional<std::int64_t> millihertz);
[[nodiscard]] std::string
quotes_frame(std::string_view id, UnixNanos ts, std::span<const Quote> quotes);
[[nodiscard]] std::string book_frame(std::string_view id, UnixNanos ts, const Book& book);
[[nodiscard]] std::string
greeks_frame(std::string_view id, UnixNanos ts, std::span<const Greeks> greeks);
[[nodiscard]] std::string
funding_frame(std::string_view id, UnixNanos ts, std::span<const Funding> funding);
[[nodiscard]] std::string
positions_frame(std::string_view id, UnixNanos ts, std::span<const Position> positions);
[[nodiscard]] std::string health_frame(std::string_view id, UnixNanos ts, const Health& health);

// The exec ring carries order events and nothing else, so a fill is the Filled
// event seen through the Fill shape rather than a second record.
[[nodiscard]] std::string order_event_frame(std::string_view id,
                                            std::uint64_t seq,
                                            const raikiri::OrderEvent& event,
                                            const InstrumentBase& base);
[[nodiscard]] std::string fill_frame(std::string_view id,
                                     std::uint64_t seq,
                                     const raikiri::OrderEvent& event,
                                     const InstrumentBase& base);

[[nodiscard]] std::string gap_frame(std::string_view id, std::uint64_t missed);
[[nodiscard]] std::string problem_frame(std::string_view id, const Problem& problem);

// -------------------------------------------------------------- stream ----

// SINGLE-THREADED AND POLL-DRIVEN, like Server beside it: the caller owns the
// loop and nothing here blocks. Boost stays behind Impl so a consumer of this
// library never inherits the dependency.
//
// It binds its own port rather than sharing admind's: an upgrade served from
// the HTTP acceptor needs a hand-off Server does not have yet.
class Stream {
public:
    Stream(const Registry& registry, const Clock& clock, std::string address, std::uint16_t port);
    ~Stream();
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    [[nodiscard]] bool start();

    // One pass: sample what is due, then run whatever the reactor has ready.
    // Returns the work done, so a caller that got zero knows it may sleep.
    std::size_t poll() noexcept;

    void stop() noexcept;

    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] std::size_t clients() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace raikiri::admin
