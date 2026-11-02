#pragma once

#include <raikiri/sdk/decimal.hpp>
#include <raikiri/sdk/models.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::sdk {

inline constexpr std::string_view k_paper_url = "http://127.0.0.1:8900";

enum class ErrorKind : std::uint8_t {
    Transport,        // no answer from the server
    Decode,           // an answer this library cannot read
    Config,           // bad arguments, before anything was sent
    Authentication,   // key, signature or replay
    TimestampSkew,    // this clock is outside the window; resynced and retried once before you see it
    PermissionDenied, // the key lacks read, trade or algo
    OutOfScope,       // the key is not allowed that account, symbol or address
    NotFound,         // no such record, or another client's
    Conflict,         // the record is not in a state for that
    OutcomeUnknown,   // it may have happened; look at your orders before sending it again with a new key
    IdempotencyMismatch,
    OrderRejected, // refused before it was queued: off tick, off lot, unknown instrument
    AlgoLost,      // the execution service restarted and forgot this algo
    BadRequest,
    RateLimited,
    Server,
};

struct Error {
    ErrorKind kind{ErrorKind::Transport};
    int status{0};
    std::string code{}; // stable; title and detail may change
    std::string title{};
    std::string detail{};
    std::string request_id{};
    std::optional<std::chrono::seconds> retry_after{};

    [[nodiscard]] std::string message() const;
};

template <typename T> using Result = std::expected<T, Error>;

struct RequestInfo {
    std::string_view method{};
    std::string_view target{};
    int attempt{0};
    std::string_view idempotency_key{};
};

struct ResponseInfo {
    std::string_view method{};
    std::string_view target{};
    int attempt{0};
    int status{0};
    std::string_view request_id{};
    bool replayed{false};
    std::optional<std::int64_t> rate_limit_remaining{};
};

// no secret and no signature ever reach these
struct Hooks {
    std::function<void(const RequestInfo&)> on_request{};
    std::function<void(const ResponseInfo&)> on_response{};
};

struct Config {
    std::string url{k_paper_url};
    std::string key{};
    std::string secret{};
    std::chrono::milliseconds timeout{10'000};
    int retries{3};
    Hooks hooks{};

    // RAIKIRI_API_URL (the paper endpoint when unset), RAIKIRI_API_KEY, RAIKIRI_API_SECRET
    [[nodiscard]] static Result<Config> from_env();
};

struct PlaceOptions {
    TimeInForce tif{TimeInForce::Gtc};
    bool post_only{false};
    bool reduce_only{false};
    std::optional<std::string> account{};      // taken from the Instrument overload when absent
    std::optional<std::string> external_ref{}; // up to 40 characters; comes back on the order
    std::optional<std::string> idempotency_key{};
};

struct OrderFilter {
    std::optional<bool> open{};
    std::optional<std::string> account{};
    std::optional<std::string> instrument_id{};
    std::optional<std::uint64_t> since_ns{};
    std::optional<std::string> cursor{};
    std::optional<int> limit{};
};

struct FillFilter {
    std::optional<std::string> account{};
    std::optional<std::string> instrument_id{};
    std::optional<std::uint64_t> since_ns{};
    std::optional<std::string> cursor{};
    std::optional<int> limit{};
};

struct AlgoSize {
    std::optional<Decimal> qty{};
    std::optional<Decimal> notional{};
};

struct AlgoOptions {
    std::optional<Decimal> limit{};
    std::string market{"perp"};
    std::optional<std::uint32_t> jitter_bp{};
    std::optional<std::uint32_t> catchup_pct{};
    std::optional<Decimal> max_child_notional{};
    std::optional<std::string> external_ref{};
    std::optional<std::string> idempotency_key{};
};

// subscriptions; market channels hold instrument ids, the rest are this client's own
struct Subscription {
    std::vector<std::string> quotes{};
    std::optional<std::string> book{};
    std::optional<int> levels{};
    std::vector<std::string> greeks{};
    std::vector<std::string> funding{};
    std::vector<std::string> ticker{};
    std::optional<int> hz{};
    bool orders{false};
    bool fills{false};
    bool positions{false};
    bool balances{false};
    bool algos{false};
};

// each runs on the client's one io thread; a slow handler delays the others
struct StreamHandlers {
    std::function<void(const Quote&)> on_quote{};
    std::function<void(const Book&)> on_book{};
    std::function<void(const Greeks&)> on_greeks{};
    std::function<void(const Funding&)> on_funding{};
    std::function<void(const Ticker&)> on_ticker{};
    std::function<void(const OrderEvent&, std::uint64_t seq)> on_order{};
    std::function<void(const Fill&, std::uint64_t seq)> on_fill{};
    std::function<void(const Position&, std::uint64_t seq)> on_position{};
    std::function<void(const AccountBalances&, std::uint64_t seq)> on_balance{};
    std::function<void(const AlgoOrder&, std::uint64_t seq)> on_algo{};
    std::function<void(const AlgoLost&, std::uint64_t seq)> on_algo_lost{};
    std::function<void(const Gap&)> on_gap{};
    // a refusal that reconnecting cannot cure, or a subscription the server refused
    std::function<void(const Error&)> on_error{};
};

// A websocket that reconnects, resubscribes and resumes from its last sequence by itself. Quotes, books, greeks,
// funding and tickers are the current value and may skip; the sequenced channels do not, and on_gap says when some
// could not be resent. Closes on destruction; no handler runs after close() returns, unless called from a handler.
class Stream {
public:
    Stream() noexcept;
    ~Stream();
    Stream(Stream&&) noexcept;
    Stream& operator=(Stream&&) noexcept;

    // sent now if connected, and again after each reconnect
    void subscribe(const Subscription& subscription);
    void close();

    [[nodiscard]] std::uint64_t last_seq() const noexcept;
    [[nodiscard]] std::uint64_t reconnects() const noexcept;
    [[nodiscard]] bool connected() const noexcept;

    struct Core;
    explicit Stream(std::shared_ptr<Core> core) noexcept;

private:
    std::shared_ptr<Core> core_;
};

class Client;

class Orders {
public:
    [[nodiscard]] Result<OrderAck>
    limit(const Instrument& instrument, Side side, Decimal qty, Decimal price, const PlaceOptions& options = {});
    [[nodiscard]] Result<OrderAck>
    market(const Instrument& instrument, Side side, Decimal qty, const PlaceOptions& options = {});
    [[nodiscard]] Result<OrderAck>
    limit(std::string_view instrument_id, Side side, Decimal qty, Decimal price, const PlaceOptions& options = {});
    [[nodiscard]] Result<OrderAck>
    market(std::string_view instrument_id, Side side, Decimal qty, const PlaceOptions& options = {});

    // queued, not confirmed; the order can still fill meanwhile
    [[nodiscard]] Result<CancelAck> cancel(std::string_view id, const std::optional<std::string>& idempotency_key = {});

    // cancels, waits for the cancel to be confirmed, then places what remains under a new id;
    // `qty` counts what already filled
    [[nodiscard]] Result<OrderAck> amend(std::string_view id,
                                         std::optional<Decimal> price,
                                         std::optional<Decimal> qty = {},
                                         const std::optional<std::string>& idempotency_key = {});
    [[nodiscard]] Result<CancelAll> cancel_all(const std::optional<std::string>& account = {},
                                               const std::optional<std::string>& instrument_id = {},
                                               const std::optional<std::string>& idempotency_key = {});
    [[nodiscard]] Result<Order> get(std::string_view id);
    [[nodiscard]] Result<std::vector<OrderEvent>> events(std::string_view id);

    // a page can be short and still have a next_cursor
    [[nodiscard]] Result<Page<Order>> list(const OrderFilter& filter = {});

    // every page
    [[nodiscard]] Result<std::vector<Order>> all(OrderFilter filter = {});

    // polls until the order is closed
    [[nodiscard]] Result<Order> wait(std::string_view id, std::chrono::milliseconds timeout = std::chrono::seconds{10});

private:
    friend class Client;

    explicit Orders(Client* client) noexcept : client_{client} {}

    Client* client_;
};

class AccountApi {
public:
    [[nodiscard]] Result<std::vector<Account>> accounts();
    [[nodiscard]] Result<std::vector<AccountBalances>> balances(const std::optional<std::string>& account = {});
    // this client's non-flat positions, folded from its own fills
    [[nodiscard]] Result<std::vector<Position>> positions(const std::optional<std::string>& account = {},
                                                          const std::optional<std::string>& instrument_id = {});
    [[nodiscard]] Result<Page<Fill>> fills(const FillFilter& filter = {});

private:
    friend class Client;

    explicit AccountApi(Client* client) noexcept : client_{client} {}

    Client* client_;
};

class MarketApi {
public:
    [[nodiscard]] Result<Instrument> instrument(std::string_view id);
    [[nodiscard]] Result<Page<Instrument>> instruments(const std::optional<std::string>& account = {},
                                                       const std::optional<std::string>& kind = {},
                                                       const std::optional<std::string>& cursor = {},
                                                       std::optional<int> limit = {});
    [[nodiscard]] Result<std::vector<Market>> markets(const std::optional<std::string>& query = {},
                                                      std::optional<int> limit = {});

    // the one instrument with this local symbol; name an account when several list it
    [[nodiscard]] Result<Instrument> find(std::string_view symbol,
                                          const std::optional<std::string>& account = {},
                                          const std::optional<std::string>& market = {});

    // taken from the stream; for more than one, keep a stream open
    [[nodiscard]] Result<Quote> quote(std::string_view instrument_id,
                                      std::chrono::milliseconds timeout = std::chrono::seconds{5});

private:
    friend class Client;

    explicit MarketApi(Client* client) noexcept : client_{client} {}

    Client* client_;
};

class OptionsApi {
public:
    [[nodiscard]] Result<std::vector<OptionUnderlying>> underlyings();
    // `expiry` is YYYYMMDD
    [[nodiscard]] Result<OptionChain>
    chain(std::string_view underlying, std::string_view expiry, const std::optional<std::string>& venue = {});
    // the surface as json, as the server sent it
    [[nodiscard]] Result<std::string> surface(std::string_view underlying,
                                              const std::optional<std::string>& venue = {});

private:
    friend class Client;

    explicit OptionsApi(Client* client) noexcept : client_{client} {}

    Client* client_;
};

class Algos;

// a running parent order; state() is as of the last refresh, pause, resume or cancel
class Algo {
public:
    [[nodiscard]] const AlgoOrder& state() const noexcept { return state_; }

    [[nodiscard]] const std::string& id() const noexcept { return state_.id; }

    [[nodiscard]] Result<AlgoOrder> refresh();
    [[nodiscard]] Result<AlgoOrder> pause();
    [[nodiscard]] Result<AlgoOrder> resume();
    [[nodiscard]] Result<AlgoOrder> cancel();

    // calls `on_change` each time the parent changes, until it is done; returns the final state
    [[nodiscard]] Result<AlgoOrder> watch(const std::function<void(const AlgoOrder&)>& on_change,
                                          std::chrono::milliseconds interval = std::chrono::seconds{1});

private:
    friend class Algos;

    Algo(Algos* algos, AlgoOrder state) : algos_{algos}, state_{std::move(state)} {}

    Algos* algos_;
    AlgoOrder state_;
};

class Algos {
public:
    [[nodiscard]] Result<Algo> twap(std::string_view symbol,
                                    Side side,
                                    AlgoSize size,
                                    std::uint64_t horizon_s,
                                    std::uint32_t slices,
                                    const AlgoOptions& options = {});
    // `profile` is flat, u or recorded
    [[nodiscard]] Result<Algo> vwap(std::string_view symbol,
                                    Side side,
                                    AlgoSize size,
                                    std::uint64_t horizon_s,
                                    std::uint32_t slices,
                                    std::string profile = "flat",
                                    const AlgoOptions& options = {});
    // sent at once across the venues that list the symbol, best price after fees first
    [[nodiscard]] Result<Algo> sor(std::string_view symbol, Side side, AlgoSize size, const AlgoOptions& options = {});
    // `pov_bp` of market volume; 1000 is 10 percent
    [[nodiscard]] Result<Algo> pov(std::string_view symbol,
                                   Side side,
                                   AlgoSize size,
                                   std::uint32_t pov_bp,
                                   std::uint64_t horizon_s = 0,
                                   const AlgoOptions& options = {});
    [[nodiscard]] Result<AlgoOrder> get(std::string_view id);
    [[nodiscard]] Result<Algo> attach(std::string_view id);
    [[nodiscard]] Result<std::vector<AlgoOrder>> list();
    [[nodiscard]] Result<AlgoOrder> pause(std::string_view id);
    [[nodiscard]] Result<AlgoOrder> resume(std::string_view id);
    [[nodiscard]] Result<AlgoOrder> cancel(std::string_view id);

private:
    friend class Client;

    explicit Algos(Client* client) noexcept : client_{client} {}

    Client* client_;
};

// One call at a time per Client over one kept-alive connection; calls from several threads queue on a mutex.
// Reads, and mutations with their idempotency key, are retried on network errors, 429 and 503. A call the server
// may have carried out without answering comes back as OutcomeUnknown instead of being sent twice.
class Client {
public:
    [[nodiscard]] static Result<Client> create(Config config);
    [[nodiscard]] static Result<Client> from_env();

    ~Client();
    Client(Client&&) noexcept;
    Client& operator=(Client&&) noexcept;

    [[nodiscard]] Orders& orders() noexcept { return orders_; }

    [[nodiscard]] AccountApi& account() noexcept { return account_; }

    [[nodiscard]] MarketApi& market() noexcept { return market_; }

    [[nodiscard]] OptionsApi& options() noexcept { return options_; }

    [[nodiscard]] Algos& algos() noexcept { return algos_; }

    [[nodiscard]] Result<Stream> stream(const Subscription& subscription, StreamHandlers handlers);

    // a call this library has no method for, such as an extension under /x/; signed and retried like the rest;
    // `path` starts at /, `query` is already encoded, and the answer is the server's json
    [[nodiscard]] Result<std::string> call(std::string_view method,
                                           std::string_view path,
                                           std::string_view query = {},
                                           std::string_view body = {},
                                           const std::optional<std::string>& idempotency_key = {});

    [[nodiscard]] Result<Whoami> whoami();
    [[nodiscard]] Result<std::uint64_t> server_time_ms();

    // measures this machine's offset from the server; it also happens by itself on a skew refusal
    [[nodiscard]] Result<void> sync_clock();

    struct Impl;

    [[nodiscard]] Impl& impl() noexcept { return *impl_; }

private:
    explicit Client(std::unique_ptr<Impl> impl) noexcept;
    void rebind() noexcept;

    std::unique_ptr<Impl> impl_;
    Orders orders_;
    AccountApi account_;
    MarketApi market_;
    OptionsApi options_;
    Algos algos_;
};

// for a query value given to Client::call
[[nodiscard]] std::string percent_encoded(std::string_view text);

// lowercase hex hmac-sha256 of "timestamp\nMETHOD\npath\nquery\nbody"; the python package computes the same
[[nodiscard]] std::string sign_request(std::string_view secret,
                                       std::string_view timestamp_ms,
                                       std::string_view method,
                                       std::string_view path,
                                       std::string_view query,
                                       std::string_view body);

} // namespace raikiri::sdk
