#pragma once

#include <raikiri/sdk/client.hpp>
#include <raikiri/sdk/glaze.hpp>

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <glaze/glaze.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace raikiri::sdk::internal {

inline constexpr glz::opts k_lenient{.error_on_unknown_keys = false};
inline constexpr glz::opts k_write{.skip_null_members = true};

inline constexpr std::string_view k_key_header = "X-Raikiri-Key";
inline constexpr std::string_view k_timestamp_header = "X-Raikiri-Timestamp";
inline constexpr std::string_view k_signature_header = "X-Raikiri-Signature";
inline constexpr std::string_view k_stream_target = "/api/v1/stream";

struct Url {
    bool tls{false};
    std::string host{};
    std::string port{};
};

[[nodiscard]] std::optional<Url> parse_url(std::string_view url);

struct HttpReply {
    int status{0};
    std::string body{};
    std::string request_id{};
    std::optional<std::chrono::seconds> retry_after{};
    std::optional<std::int64_t> rate_limit_remaining{};
    bool replayed{false};
};

struct Header {
    std::string_view name;
    std::string_view value;
};

// one connection kept alive between calls; a broken one is replaced on the next
class Transport {
public:
    Transport(Url url, std::chrono::milliseconds timeout);
    ~Transport();

    [[nodiscard]] std::expected<HttpReply, std::string> exchange(std::string_view method,
                                                                 std::string_view target,
                                                                 const std::vector<Header>& headers,
                                                                 std::string_view body);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// the one background thread a client's streams share; outlives the client while a stream holds it
class IoThread {
public:
    IoThread();
    ~IoThread();

    [[nodiscard]] boost::asio::io_context& context() noexcept { return context_; }

    [[nodiscard]] bool on_this_thread() const noexcept { return std::this_thread::get_id() == thread_.get_id(); }

private:
    boost::asio::io_context context_;
    std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> guard_;
    std::thread thread_;
};

// signs for both rest and the stream's upgrade
class Signer {
public:
    Signer(std::string key, std::string secret) : key_{std::move(key)}, secret_{std::move(secret)} {}

    struct Signed {
        std::string timestamp;
        std::string signature;
    };

    [[nodiscard]] Signed
    sign(std::string_view method, std::string_view path, std::string_view query, std::string_view body);

    [[nodiscard]] const std::string& key() const noexcept { return key_; }

    void set_offset_ms(std::int64_t offset) noexcept { offset_ms_.store(offset, std::memory_order_relaxed); }

private:
    std::string key_;
    std::string secret_;
    std::atomic<std::int64_t> offset_ms_{0};
    std::atomic<std::int64_t> last_ms_{0};
};

// what the clock differs by; empty when the server did not say
[[nodiscard]] std::optional<std::int64_t> measure_offset_ms(const Url& url, std::chrono::milliseconds timeout);

[[nodiscard]] Error decode_error(std::string_view what);
[[nodiscard]] Error error_of_problem(int status,
                                     std::string_view body,
                                     std::string request_id,
                                     std::optional<std::chrono::seconds> retry_after);

[[nodiscard]] std::string percent_encode(std::string_view text);

std::shared_ptr<Stream::Core> open_stream(std::shared_ptr<IoThread> io,
                                          Url url,
                                          std::shared_ptr<Signer> signer,
                                          const Subscription& subscription,
                                          StreamHandlers handlers);

} // namespace raikiri::sdk::internal
