// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Deterministic resource attribution. Attribution never invents a number and
// never splits an unknown total: when the inputs are insufficient the report
// says so with an explicit status instead of a plausible-looking share.
//
// Stale path generations cannot support current attribution. A generation whose
// evidence is not fresh, or which is no longer the flow's current generation,
// is refused under the default policy.

#ifndef FLOWOBS_ATTRIBUTION_HPP
#define FLOWOBS_ATTRIBUTION_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "flowobs/counters.hpp"
#include "flowobs/export.hpp"
#include "flowobs/flow.hpp"
#include "flowobs/identity.hpp"
#include "flowobs/policy.hpp"

namespace flowobs {

enum class AttributionStatus : std::uint8_t {
  Ok = 0,
  FlowNotFound = 1,
  GenerationNotFound = 2,
  StaleGeneration = 3,        // evidence for this generation is not fresh
  NotCurrentGeneration = 4,   // a newer generation exists for the flow
  NoPathCorrelation = 5,      // the generation has no accepted path evidence
  UnsupportedByMetadata = 6,  // a chosen method needs metadata that is absent
  UnresolvedConflict = 7,     // sources disagree, so no number may be published
  LimitExceeded = 8,
};

FLOWOBS_PUBLIC std::string_view to_string(AttributionStatus status) noexcept;

// One attributed target.
struct FLOWOBS_PUBLIC AttributedTarget final {
  enum class Kind : std::uint8_t {
    Flow = 0,
    EndpointA = 1,
    EndpointB = 2,
    Path = 3,
    Link = 4,
    Queue = 5,
  };

  Kind kind = Kind::Flow;
  std::uint32_t index = 0;       // position in the path, for links and queues
  LinkId link{};
  QueueId queue{};
  EndpointId endpoint{};
  Consumption consumption{};
  // Present only when the runtime had verified capacity metadata; absent means
  // the share is a plain division, not a capacity-weighted one.
  std::optional<std::uint64_t> capacity_units;
  // The exact integer share before rounding, as numerator/denominator. Kept so
  // that explanations can show why a target received what it did.
  std::uint64_t share_numerator = 0;
  std::uint64_t share_denominator = 0;

  [[nodiscard]] std::string describe() const;
};

struct FLOWOBS_PUBLIC AttributionReport final {
  FlowId flow{};
  GenerationId generation{};
  AttributionStatus status = AttributionStatus::Ok;
  std::string status_detail;
  AttributionPolicy::Method method = AttributionPolicy::Method::EndpointOnly;

  bool generation_is_current = false;
  Freshness generation_freshness = Freshness::Unknown;
  Consumption total{};

  std::vector<AttributedTarget> targets;
  std::vector<std::string> notes;

  [[nodiscard]] bool ok() const noexcept { return status == AttributionStatus::Ok; }
  [[nodiscard]] std::string describe() const;
};

// Pure attribution over a reconciled generation view.
FLOWOBS_PUBLIC AttributionReport attribute_generation(
    const FlowSnapshot& flow, const GenerationView& generation, const AttributionPolicy& policy,
    std::size_t max_targets);

}  // namespace flowobs

#endif  // FLOWOBS_ATTRIBUTION_HPP
