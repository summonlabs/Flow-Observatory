// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "flowobs/flow.hpp"

#include <string>

namespace flowobs {

std::string Anomaly::describe() const {
  std::string out;
  out.reserve(160);
  out.append(flowobs::to_string(kind));
  out.append(" at=");
  out.append(std::to_string(at.nanos()));
  if (observed_at.known()) {
    out.append(" observed_at=");
    out.append(std::to_string(observed_at.nanos()));
  }
  if (generation.valid()) {
    out.append(" gen=");
    out.append(generation.to_string());
  }
  if (source.valid()) {
    out.append(" src=");
    out.append(flowobs::to_string(source));
  }
  if (!detail.empty()) {
    out.append(" detail=");
    out.append(detail);
  }
  return out;
}

std::string LifecycleEvent::describe() const {
  std::string out;
  out.reserve(160);
  out.append(flowobs::to_string(from));
  out.append(" -> ");
  out.append(flowobs::to_string(to));
  out.append(" cause=");
  out.append(flowobs::to_string(cause));
  out.append(" at=");
  out.append(std::to_string(at.nanos()));
  if (observed_at.known()) {
    out.append(" observed_at=");
    out.append(std::to_string(observed_at.nanos()));
  }
  if (generation.valid()) {
    out.append(" gen=");
    out.append(generation.to_string());
  }
  if (!detail.empty()) {
    out.append(" detail=");
    out.append(detail);
  }
  return out;
}

const GenerationView* FlowSnapshot::find_generation(GenerationId generation_id) const noexcept {
  for (const GenerationView& view : generations) {
    if (view.generation.generation() == generation_id) {
      return &view;
    }
  }
  return nullptr;
}

std::string FlowSnapshot::one_line() const {
  std::string out;
  out.reserve(256);
  out.append(flowobs::to_string(id));
  out.append(" key=");
  out.append(canonical_key);
  out.append(" gen=");
  out.append(std::to_string(current_generation.value()));
  out.append(" state=");
  out.append(flowobs::to_string(state));
  out.append(" cause=");
  out.append(flowobs::to_string(cause));
  out.append(" freshness=");
  out.append(flowobs::to_string(freshness));
  out.append(" consistency=");
  out.append(flowobs::to_string(consistency));
  out.append(" visibility=");
  out.append(flowobs::to_string(visibility));
  out.append(" total{");
  out.append(total.describe());
  out.push_back('}');
  if (restored_from_journal) {
    out.append(" restored=true");
  }
  return out;
}

}  // namespace flowobs
