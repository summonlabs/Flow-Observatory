// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "flowobs/attribution.hpp"

#include <string>
#include <utility>

#include "flowobs/checked.hpp"

namespace flowobs {

std::string_view to_string(AttributionStatus status) noexcept {
  switch (status) {
    case AttributionStatus::Ok: return "ok";
    case AttributionStatus::FlowNotFound: return "flow-not-found";
    case AttributionStatus::GenerationNotFound: return "generation-not-found";
    case AttributionStatus::StaleGeneration: return "stale-generation";
    case AttributionStatus::NotCurrentGeneration: return "not-current-generation";
    case AttributionStatus::NoPathCorrelation: return "no-path-correlation";
    case AttributionStatus::UnsupportedByMetadata: return "unsupported-by-metadata";
    case AttributionStatus::UnresolvedConflict: return "unresolved-conflict";
    case AttributionStatus::LimitExceeded: return "limit-exceeded";
  }
  return "invalid";
}

std::string AttributedTarget::describe() const {
  std::string out;
  out.reserve(160);
  switch (kind) {
    case Kind::Flow: out.append("flow:"); out.append(flowobs::to_string(FlowId::invalid())); break;
    case Kind::EndpointA: out.append("endpoint-a"); break;
    case Kind::EndpointB: out.append("endpoint-b"); break;
    case Kind::Path: out.append("path"); break;
    case Kind::Link: out.append("link["); out.append(std::to_string(index)); out.push_back(']'); break;
    case Kind::Queue: out.append("queue["); out.append(std::to_string(index)); out.push_back(']'); break;
  }
  if (link.valid()) {
    out.push_back(' ');
    out.append(flowobs::to_string(link));
  }
  if (queue.valid()) {
    out.push_back(' ');
    out.append(flowobs::to_string(queue));
  }
  if (endpoint.valid()) {
    out.push_back(' ');
    out.append(flowobs::to_string(endpoint));
  }
  out.append(" consumption{");
  out.append(consumption.describe());
  out.push_back('}');
  if (capacity_units.has_value()) {
    out.append(" capacity=");
    out.append(std::to_string(*capacity_units));
  }
  return out;
}

std::string AttributionReport::describe() const {
  std::string out;
  out.reserve(256);
  out.append("attribution ");
  out.append(flowobs::to_string(flow));
  out.append("/");
  out.append(flowobs::to_string(generation));
  out.append(" status=");
  out.append(flowobs::to_string(status));
  if (!status_detail.empty()) {
    out.append(" (");
    out.append(status_detail);
    out.push_back(')');
  }
  out.append(" targets=");
  out.append(std::to_string(targets.size()));
  out.append(" total{");
  out.append(total.describe());
  out.push_back('}');
  for (const std::string& note : notes) {
    out.append("\n  note: ");
    out.append(note);
  }
  for (const AttributedTarget& target : targets) {
    out.append("\n  ");
    out.append(target.describe());
  }
  return out;
}

namespace {

// Largest-remainder distribution of `total` across `weights`. Integer only:
// the result always sums exactly to `total`, and the remainder is assigned in
// index order so the split is reproducible.
void distribute(std::uint64_t total, const std::vector<std::uint64_t>& weights,
                std::vector<std::uint64_t>& shares) {
  shares.assign(weights.size(), 0);
  if (weights.empty()) {
    return;
  }
  std::uint64_t weight_sum = 0;
  for (const std::uint64_t weight : weights) {
    const Checked<std::uint64_t> sum = checked_add(weight_sum, weight);
    if (sum.overflow) {
      weight_sum = std::numeric_limits<std::uint64_t>::max();
      break;
    }
    weight_sum = sum.value;
  }
  if (weight_sum == 0) {
    // Equal split. The integer remainder goes to the lowest indices.
    const std::uint64_t base = total / static_cast<std::uint64_t>(weights.size());
    std::uint64_t remainder = total % static_cast<std::uint64_t>(weights.size());
    for (std::size_t i = 0; i < weights.size(); ++i) {
      shares[i] = base + (remainder > 0 ? 1u : 0u);
      if (remainder > 0) {
        --remainder;
      }
    }
    return;
  }
  std::uint64_t assigned = 0;
  std::vector<std::uint64_t> remainders(weights.size(), 0);
  for (std::size_t i = 0; i < weights.size(); ++i) {
    const Checked<std::uint64_t> product = checked_mul(total, weights[i]);
    const std::uint64_t scaled =
        product.overflow ? std::numeric_limits<std::uint64_t>::max() / weight_sum
                         : product.value / weight_sum;
    shares[i] = scaled;
    assigned += scaled;
    remainders[i] = product.overflow ? 0 : product.value % weight_sum;
  }
  if (assigned > total) {
    // Saturation in the multiplication path can only overshoot in pathological
    // cases; clamping keeps the invariant "shares sum to total" true.
    std::uint64_t excess = assigned - total;
    for (std::size_t i = shares.size(); i-- > 0 && excess > 0;) {
      const std::uint64_t take = shares[i] < excess ? shares[i] : excess;
      shares[i] -= take;
      excess -= take;
    }
    return;
  }
  std::uint64_t remainder = total - assigned;
  while (remainder > 0) {
    std::size_t best = 0;
    bool found = false;
    for (std::size_t i = 0; i < weights.size(); ++i) {
      if (!found || remainders[i] > remainders[best] ||
          (remainders[i] == remainders[best] && i < best)) {
        best = i;
        found = true;
      }
    }
    if (!found) {
      break;
    }
    ++shares[best];
    remainders[best] = 0;
    --remainder;
  }
}

}  // namespace

AttributionReport attribute_generation(const FlowSnapshot& flow,
                                       const GenerationView& generation,
                                       const AttributionPolicy& policy,
                                       std::size_t max_targets) {
  AttributionReport report;
  report.flow = flow.id;
  report.generation = generation.generation.generation();
  report.method = policy.method;
  report.generation_is_current = generation.is_current;
  report.generation_freshness = generation.freshness;

  const auto fail = [&report](AttributionStatus status, std::string detail) {
    report.status = status;
    report.status_detail = std::move(detail);
    return report;
  };

  if (policy.require_current_generation && !generation.is_current) {
    return fail(AttributionStatus::NotCurrentGeneration,
                "the flow has advanced beyond this generation");
  }
  if (policy.require_fresh_generation && generation.freshness != Freshness::Fresh) {
    return fail(AttributionStatus::StaleGeneration,
                "generation freshness is " +
                    std::string(flowobs::to_string(generation.freshness)) +
                    "; stale path generations cannot support current attribution");
  }
  if (generation.unresolved_conflict) {
    return fail(AttributionStatus::UnresolvedConflict,
                generation.conflict_detail.empty() ? "sources disagree"
                                                   : generation.conflict_detail);
  }
  report.total = generation.total;

  AttributedTarget flow_target;
  flow_target.kind = AttributedTarget::Kind::Flow;
  flow_target.index = 0;
  flow_target.consumption = generation.total;
  report.targets.push_back(std::move(flow_target));

  if (generation.endpoint_a.valid() || generation.endpoint_b.valid()) {
    AttributedTarget a;
    a.kind = AttributedTarget::Kind::EndpointA;
    a.endpoint = generation.endpoint_a;
    a.consumption = generation.forward;
    report.targets.push_back(std::move(a));

    AttributedTarget b;
    b.kind = AttributedTarget::Kind::EndpointB;
    b.endpoint = generation.endpoint_b;
    b.consumption = generation.reverse;
    if (!generation.endpoint_b.valid()) {
      b.consumption = Consumption{};
      report.notes.push_back(
          "the reverse endpoint was never named by any accepted observation");
    }
    report.targets.push_back(std::move(b));
  } else {
    report.notes.push_back("no endpoint correlation was declared by any accepted observation");
  }

  if (policy.method == AttributionPolicy::Method::EndpointOnly) {
    if (!report.targets.empty() && report.targets.size() > max_targets) {
      return fail(AttributionStatus::LimitExceeded, "target count exceeds the configured bound");
    }
    return report;
  }

  if (!generation.path.has_value() || generation.links.empty()) {
    return fail(AttributionStatus::NoPathCorrelation,
                "the chosen method needs a path with hop evidence and none was accepted");
  }
  if (generation.links.size() > max_targets) {
    return fail(AttributionStatus::LimitExceeded, "path exceeds the configured target bound");
  }

  std::vector<std::uint64_t> weights(generation.links.size(), 1);
  if (policy.method == AttributionPolicy::Method::ProportionalToCapacity) {
    if (generation.hop_capacity_units.size() != generation.links.size()) {
      return fail(AttributionStatus::UnsupportedByMetadata,
                  "capacity metadata is missing or its length does not match the hop count");
    }
    weights = generation.hop_capacity_units;
    for (const std::uint64_t weight : weights) {
      if (weight == 0) {
        return fail(AttributionStatus::UnsupportedByMetadata,
                    "capacity metadata contains a zero weight");
      }
    }
  }

  std::vector<std::uint64_t> byte_shares;
  std::vector<std::uint64_t> packet_shares;
  std::vector<std::uint64_t> retx_shares;
  distribute(generation.total.bytes, weights, byte_shares);
  distribute(generation.total.packets, weights, packet_shares);
  distribute(generation.total.retransmissions, weights, retx_shares);

  for (std::size_t i = 0; i < generation.links.size(); ++i) {
    AttributedTarget target;
    target.kind = AttributedTarget::Kind::Link;
    target.index = static_cast<std::uint32_t>(i);
    target.link = generation.links[i];
    target.consumption.bytes = byte_shares[i];
    target.consumption.packets = packet_shares[i];
    target.consumption.retransmissions = retx_shares[i];
    target.consumption.unknown_intervals = generation.total.unknown_intervals;
    target.consumption.saturated = generation.total.saturated;
    target.consumption.bytes_unsupported = generation.total.bytes_unsupported;
    target.consumption.packets_unsupported = generation.total.packets_unsupported;
    target.consumption.retransmissions_unsupported =
        generation.total.retransmissions_unsupported;
    std::uint64_t weight_sum = 0;
    for (const std::uint64_t weight : weights) {
      weight_sum += weight;
    }
    target.share_numerator = weights[i];
    target.share_denominator = weight_sum;
    if (policy.method == AttributionPolicy::Method::ProportionalToCapacity) {
      target.capacity_units = weights[i];
    }
    report.targets.push_back(std::move(target));

    if (i < generation.queues.size()) {
      AttributedTarget queue_target;
      queue_target.kind = AttributedTarget::Kind::Queue;
      queue_target.index = static_cast<std::uint32_t>(i);
      queue_target.queue = generation.queues[i];
      queue_target.consumption.bytes = byte_shares[i];
      queue_target.consumption.packets = packet_shares[i];
      queue_target.consumption.retransmissions = retx_shares[i];
      queue_target.consumption.unknown_intervals = generation.total.unknown_intervals;
      queue_target.consumption.saturated = generation.total.saturated;
      queue_target.share_numerator = weights[i];
      queue_target.share_denominator = weight_sum;
      report.targets.push_back(std::move(queue_target));
    }
  }
  if (generation.queues.size() > generation.links.size()) {
    report.notes.push_back("more queues than hops were declared; the extra queues carry no share");
  }
  if (generation.total.unknown_intervals > 0) {
    report.notes.push_back("the total is a lower bound: " +
                           std::to_string(generation.total.unknown_intervals) +
                           " interval(s) were never observed");
  }
  if (report.targets.size() > max_targets) {
    return fail(AttributionStatus::LimitExceeded, "target count exceeds the configured bound");
  }
  return report;
}

}  // namespace flowobs
