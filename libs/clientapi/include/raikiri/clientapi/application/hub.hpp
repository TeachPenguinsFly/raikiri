#pragma once

// one ring per client: a key's sessions read the same sequence, and each filters it by that key's scope

#include <raikiri/clientapi/domain/key.hpp>
#include <raikiri/clientapi/domain/trading.hpp>

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace raikiri::clientapi {

enum class Channel : std::uint8_t { Orders = 1, Fills = 2, Positions = 4, Balances = 8, Algos = 16 };

[[nodiscard]] constexpr std::uint8_t mask_of(Channel channel) noexcept {
    return static_cast<std::uint8_t>(channel);
}

struct AlgoLost {
    std::string id{};
    std::string reason{};
};

// events were lost before they reached this ring; refetch over rest
struct FeedGap {
    std::string reason{};
};

using PrivatePayload = std::variant<OrderEventRow, FillRow, PositionRow, AccountBalances, ParentRow, AlgoLost, FeedGap>;

struct PrivateEvent {
    std::uint64_t seq{0};
    std::uint8_t channels{0}; // zero reaches every subscriber
    std::string account{};
    std::string symbol{};
    PrivatePayload payload{};
};

class StreamSubscriber {
public:
    StreamSubscriber() = default;
    StreamSubscriber(const StreamSubscriber&) = delete;
    StreamSubscriber& operator=(const StreamSubscriber&) = delete;
    virtual ~StreamSubscriber() = default;

    // called on the publisher's thread; an implementation hands the event to its own
    virtual void deliver(std::shared_ptr<const PrivateEvent> event) = 0;
};

struct ResumePlan {
    bool gap{false};
    std::uint64_t gap_from{0}; // first sequence that can no longer be sent
    std::uint64_t gap_to{0};   // last one
    std::vector<std::shared_ptr<const PrivateEvent>> events{};
    std::uint64_t head{0};
};

[[nodiscard]] bool visible_to(const ApiKey& key, const PrivateEvent& event) noexcept;

class StreamHub {
public:
    // `epoch` names this ring's life; a sequence from another epoch means nothing here
    StreamHub(std::string epoch, std::size_t ring_capacity) noexcept
        : epoch_{std::move(epoch)}, capacity_{ring_capacity} {}

    [[nodiscard]] const std::string& epoch() const noexcept { return epoch_; }

    void publish(const std::string& client_id,
                 std::uint8_t channels,
                 std::string_view account,
                 std::string_view symbol,
                 PrivatePayload payload);

    // to every client with a session open
    void publish_connected(std::uint8_t channels, std::string_view account, const PrivatePayload& payload);

    void attach(const std::string& client_id, std::weak_ptr<StreamSubscriber> subscriber);

    [[nodiscard]] ResumePlan since(const std::string& client_id, std::string_view epoch, std::uint64_t from);
    [[nodiscard]] std::uint64_t head(const std::string& client_id);
    [[nodiscard]] bool any_connected();

    [[nodiscard]] bool acquire_stream(const ApiKey& key);
    void release_stream(const std::string& key_id);

private:
    struct Ring {
        std::uint64_t seq{0};
        std::deque<std::shared_ptr<const PrivateEvent>> events{};
        std::vector<std::weak_ptr<StreamSubscriber>> subscribers{};
    };

    void publish_locked(const std::string& client_id,
                        Ring& ring,
                        std::uint8_t channels,
                        std::string_view account,
                        std::string_view symbol,
                        PrivatePayload payload);

    std::string epoch_;
    std::size_t capacity_;
    std::mutex mutex_;
    std::map<std::string, Ring> rings_;
    std::unordered_map<std::string, std::uint32_t> streams_;
};

} // namespace raikiri::clientapi
