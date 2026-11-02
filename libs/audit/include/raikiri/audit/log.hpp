#pragma once

#include <raikiri/common/time.hpp>

#include <string_view>

namespace raikiri::audit {

// the views need only outlive append
struct Entry {
    UnixNanos ts{};
    std::string_view actor{};
    std::string_view action{};
    std::string_view subject{};
    std::string_view detail_json{};
};

class Sink {
public:
    Sink() = default;
    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;
    virtual ~Sink() = default;

    virtual void append(const Entry& entry) = 0;
};

} // namespace raikiri::audit
