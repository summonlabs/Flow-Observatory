// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Strongly typed identities. Every domain object that can be referred to by
// evidence carries a distinct type, so that a FlowId can never be passed where
// a PathId is expected.
//
// Two identity families exist:
//
//  * Numeric identities (EpochId, IncarnationId, GenerationId, RevisionId,
//    SequenceNo) are opaque counters. Their value domain is defined by the
//    producer and fenced by the runtime.
//
//  * Named identities (FlowId, EndpointId, PathId, LinkId, QueueId, SourceId)
//    are derived from a caller supplied canonical key through a versioned,
//    domain separated hash. The canonical key is retained so that explanations
//    and exports are human readable. A collision (two distinct keys mapping to
//    the same identity) is detected and reported; it is never merged silently.

#ifndef FLOWOBS_IDENTITY_HPP
#define FLOWOBS_IDENTITY_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "flowobs/error.hpp"
#include "flowobs/export.hpp"

namespace flowobs {

// ---------------------------------------------------------------------------
// Numeric identity template.
// ---------------------------------------------------------------------------

template <class Tag>
class NumericId final {
 public:
  using value_type = std::uint64_t;
  using tag_type = Tag;

  constexpr NumericId() noexcept = default;
  constexpr explicit NumericId(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr NumericId from_value(std::uint64_t v) noexcept {
    return NumericId(v);
  }
  [[nodiscard]] static constexpr NumericId invalid() noexcept { return NumericId(); }

  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
  [[nodiscard]] constexpr bool is_valid() const noexcept { return valid(); }
  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

  // Monotonic successor. Saturates at UINT64_MAX rather than wrapping; callers
  // that care compare against NumericId::max_value().
  [[nodiscard]] constexpr NumericId next() const noexcept {
    return value_ == UINT64_MAX ? NumericId(value_) : NumericId(value_ + 1u);
  }
  [[nodiscard]] static constexpr NumericId max_value() noexcept {
    return NumericId(UINT64_MAX);
  }

  friend constexpr bool operator==(NumericId a, NumericId b) noexcept {
    return a.value_ == b.value_;
  }
  friend constexpr bool operator!=(NumericId a, NumericId b) noexcept {
    return a.value_ != b.value_;
  }
  friend constexpr bool operator<(NumericId a, NumericId b) noexcept {
    return a.value_ < b.value_;
  }
  friend constexpr bool operator>(NumericId a, NumericId b) noexcept {
    return a.value_ > b.value_;
  }
  friend constexpr bool operator<=(NumericId a, NumericId b) noexcept {
    return a.value_ <= b.value_;
  }
  friend constexpr bool operator>=(NumericId a, NumericId b) noexcept {
    return a.value_ >= b.value_;
  }

 private:
  std::uint64_t value_ = 0;
};

// ---------------------------------------------------------------------------
// Named identity template.
// ---------------------------------------------------------------------------

template <class Tag>
class OpaqueId final {
 public:
  using value_type = std::uint64_t;
  using tag_type = Tag;

  constexpr OpaqueId() noexcept = default;
  constexpr explicit OpaqueId(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr OpaqueId from_value(std::uint64_t v) noexcept {
    return OpaqueId(v);
  }
  [[nodiscard]] static constexpr OpaqueId invalid() noexcept { return OpaqueId(); }

  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
  [[nodiscard]] constexpr bool is_valid() const noexcept { return valid(); }
  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

  friend constexpr bool operator==(OpaqueId a, OpaqueId b) noexcept {
    return a.value_ == b.value_;
  }
  friend constexpr bool operator!=(OpaqueId a, OpaqueId b) noexcept {
    return a.value_ != b.value_;
  }
  friend constexpr bool operator<(OpaqueId a, OpaqueId b) noexcept {
    return a.value_ < b.value_;
  }
  friend constexpr bool operator>(OpaqueId a, OpaqueId b) noexcept {
    return a.value_ > b.value_;
  }
  friend constexpr bool operator<=(OpaqueId a, OpaqueId b) noexcept {
    return a.value_ <= b.value_;
  }
  friend constexpr bool operator>=(OpaqueId a, OpaqueId b) noexcept {
    return a.value_ >= b.value_;
  }

 private:
  std::uint64_t value_ = 0;
};

// ---------------------------------------------------------------------------
// Tag types. Each tag fixes the identity domain, the textual prefix and the
// hash domain string.
// ---------------------------------------------------------------------------

struct FlowTag {
  static constexpr std::string_view domain = "flow";
  static constexpr std::string_view prefix = "flow";
};
struct EndpointTag {
  static constexpr std::string_view domain = "endpoint";
  static constexpr std::string_view prefix = "ep";
};
struct PathTag {
  static constexpr std::string_view domain = "path";
  static constexpr std::string_view prefix = "path";
};
struct LinkTag {
  static constexpr std::string_view domain = "link";
  static constexpr std::string_view prefix = "link";
};
struct QueueTag {
  static constexpr std::string_view domain = "queue";
  static constexpr std::string_view prefix = "queue";
};
struct SourceTag {
  static constexpr std::string_view domain = "source";
  static constexpr std::string_view prefix = "src";
};
struct EpochTag {
  static constexpr std::string_view domain = "epoch";
  static constexpr std::string_view prefix = "epoch";
};
struct IncarnationTag {
  static constexpr std::string_view domain = "incarnation";
  static constexpr std::string_view prefix = "inc";
};
struct GenerationTag {
  static constexpr std::string_view domain = "generation";
  static constexpr std::string_view prefix = "gen";
};
struct RevisionTag {
  static constexpr std::string_view domain = "revision";
  static constexpr std::string_view prefix = "rev";
};
struct SequenceTag {
  static constexpr std::string_view domain = "sequence";
  static constexpr std::string_view prefix = "seq";
};
struct RuntimeEpochTag {
  static constexpr std::string_view domain = "runtime_epoch";
  static constexpr std::string_view prefix = "rt";
};

using FlowId = OpaqueId<FlowTag>;
using EndpointId = OpaqueId<EndpointTag>;
using PathId = OpaqueId<PathTag>;
using LinkId = OpaqueId<LinkTag>;
using QueueId = OpaqueId<QueueTag>;
using SourceId = OpaqueId<SourceTag>;

using EpochId = NumericId<EpochTag>;
using IncarnationId = NumericId<IncarnationTag>;
using GenerationId = NumericId<GenerationTag>;
using RevisionId = NumericId<RevisionTag>;
using SequenceNo = NumericId<SequenceTag>;

// Increments once per successful engine open against a journal. It is the
// fence that stops evidence carried over from a previous process lifetime from
// being treated as current.
using RuntimeEpoch = NumericId<RuntimeEpochTag>;

// Bump this when the hash construction changes. It is part of every journal
// header so that a journal written by an incompatible identity scheme is
// refused instead of silently reinterpreted.
inline constexpr std::uint32_t kIdentitySchemeVersion = 1;

// FNV-1a 64 over the domain string, a 0x00 separator, then the canonical key.
// The construction is fully specified in docs/semantics.md so that independent
// implementations agree. A raw hash of 0 is mapped to 0x0000'0000'0000'0001 so
// that a valid identity is never equal to the invalid identity.
FLOWOBS_PUBLIC std::uint64_t identity_hash(std::string_view domain,
                                           std::string_view key) noexcept;

template <class Tag>
[[nodiscard]] OpaqueId<Tag> make_id(std::string_view key) noexcept {
  return OpaqueId<Tag>::from_value(identity_hash(Tag::domain, key));
}

// Appends 16 lowercase hex digits (zero padded, no prefix).
FLOWOBS_PUBLIC void append_hex16(std::string& out, std::uint64_t value);
FLOWOBS_PUBLIC void append_hex(std::string& out, std::uint64_t value);

// Parses exactly 16 hex digits (no prefix, no separators).
FLOWOBS_PUBLIC bool parse_hex16(std::string_view text, std::uint64_t& out) noexcept;

template <class Tag>
[[nodiscard]] std::string id_to_string(OpaqueId<Tag> id) {
  static_assert(Tag::prefix.size() > 0, "identity tag must declare a prefix");
  std::string out;
  out.reserve(Tag::prefix.size() + 17u);
  out.append(Tag::prefix);
  out.push_back(':');
  append_hex16(out, id.value());
  return out;
}

template <class Tag>
[[nodiscard]] std::string id_to_string(NumericId<Tag> id) {
  std::string out;
  out.reserve(Tag::prefix.size() + 18u);
  out.append(Tag::prefix);
  out.push_back(':');
  out.append(std::to_string(id.value()));
  return out;
}

// Canonical, human readable form of an identity for explanations and exports.
FLOWOBS_PUBLIC std::string to_string(FlowId id);
FLOWOBS_PUBLIC std::string to_string(EndpointId id);
FLOWOBS_PUBLIC std::string to_string(PathId id);
FLOWOBS_PUBLIC std::string to_string(LinkId id);
FLOWOBS_PUBLIC std::string to_string(QueueId id);
FLOWOBS_PUBLIC std::string to_string(SourceId id);
FLOWOBS_PUBLIC std::string to_string(EpochId id);
FLOWOBS_PUBLIC std::string to_string(IncarnationId id);
FLOWOBS_PUBLIC std::string to_string(GenerationId id);
FLOWOBS_PUBLIC std::string to_string(RevisionId id);
FLOWOBS_PUBLIC std::string to_string(SequenceNo id);

// A composite identity describing which generation of which flow a piece of
// evidence belongs to. Evidence is only ever comparable to evidence with an
// equal FlowGeneration.
class FLOWOBS_PUBLIC FlowGeneration final {
 public:
  constexpr FlowGeneration() noexcept = default;
  constexpr FlowGeneration(FlowId flow, GenerationId generation) noexcept
      : flow_(flow), generation_(generation) {}

  [[nodiscard]] constexpr FlowId flow() const noexcept { return flow_; }
  [[nodiscard]] constexpr GenerationId generation() const noexcept { return generation_; }
  [[nodiscard]] constexpr bool valid() const noexcept {
    return flow_.valid() && generation_.valid();
  }

  friend constexpr bool operator==(FlowGeneration a, FlowGeneration b) noexcept {
    return a.flow_ == b.flow_ && a.generation_ == b.generation_;
  }
  friend constexpr bool operator!=(FlowGeneration a, FlowGeneration b) noexcept {
    return !(a == b);
  }
  friend constexpr bool operator<(FlowGeneration a, FlowGeneration b) noexcept {
    if (a.flow_ != b.flow_) return a.flow_ < b.flow_;
    return a.generation_ < b.generation_;
  }

  [[nodiscard]] std::string to_string() const;

 private:
  FlowId flow_{};
  GenerationId generation_{};
};

}  // namespace flowobs

namespace std {

template <class Tag>
struct hash<flowobs::OpaqueId<Tag>> {
  std::size_t operator()(flowobs::OpaqueId<Tag> id) const noexcept {
    // The identity is already a well distributed 64-bit value.
    return static_cast<std::size_t>(id.value());
  }
};

template <class Tag>
struct hash<flowobs::NumericId<Tag>> {
  std::size_t operator()(flowobs::NumericId<Tag> id) const noexcept {
    return static_cast<std::size_t>(id.value() * 0x9E3779B97F4A7C15ull);
  }
};

template <>
struct hash<flowobs::FlowGeneration> {
  std::size_t operator()(flowobs::FlowGeneration fg) const noexcept {
    return static_cast<std::size_t>(fg.flow().value() ^
                                    (fg.generation().value() * 0x9E3779B97F4A7C15ull));
  }
};

}  // namespace std

#endif  // FLOWOBS_IDENTITY_HPP
