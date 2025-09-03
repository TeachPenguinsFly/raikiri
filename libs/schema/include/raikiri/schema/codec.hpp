#pragma once

// Glue between the generated SBE codecs and the journal: encode-and-append
// on the write side, id-based dispatch support on the read side. This header
// is the only place that knows both worlds; the journal itself stays a
// byte-oriented container and the generated code stays pure codec.
//
// Frame mapping: journal frame `schema_id` carries the SBE message template
// id; `schema_version` carries the schema version. Consumers dispatch on the
// template id and may accept older versions per SBE evolution rules.

#include <raikiri/journal/writer.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <raikiri_journal/raikiri_journal.hpp>
#include <utility>

namespace raikiri::schema {

inline constexpr std::uint16_t k_schema_version = 1; // keep in sync with journal.xml

// Encodes one message on the stack and appends it. Usage:
//
//   append_msg<raikiri_journal::schema::messages::BboMsg>(writer, now_ns,
//       [&](auto m) {
//           m.instrument(types::instrumentId{bbo.instrument.packed});
//           ...
//       });
template <typename MsgTag, typename Fill>
[[nodiscard]] std::expected<std::uint64_t, journal::JournalError>
append_msg(journal::JournalWriter& writer, std::uint64_t ts_ns, Fill&& fill) noexcept {
    using Traits = sbepp::message_traits<MsgTag>;
    using Msg = typename Traits::template value_type<std::byte>;

    std::array<std::byte, 512> buf{};
    Msg msg{buf.data(), buf.size()};
    sbepp::fill_message_header(msg);
    std::forward<Fill>(fill)(msg);

    const auto used = static_cast<std::size_t>(sbepp::size_bytes(msg));
    return writer.append(static_cast<std::uint16_t>(Traits::id()),
                         k_schema_version,
                         std::span<const std::byte>{buf.data(), used},
                         ts_ns);
}

// Read-side: view a frame payload as message MsgTag. The caller has already
// matched frame.schema_id against `message_id<MsgTag>()`.
template <typename MsgTag>
[[nodiscard]] auto view_msg(std::span<const std::byte> payload) noexcept {
    using Msg = typename sbepp::message_traits<MsgTag>::template value_type<const std::byte>;
    return Msg{payload.data(), payload.size()};
}

template <typename MsgTag> [[nodiscard]] constexpr std::uint16_t message_id() noexcept {
    return static_cast<std::uint16_t>(sbepp::message_traits<MsgTag>::id());
}

} // namespace raikiri::schema
