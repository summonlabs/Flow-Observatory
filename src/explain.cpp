// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Deterministic explanations. The renderer never reads a clock, never iterates
// an unordered container and never formats a floating point number, so the same
// state always produces byte-identical text.

#include <algorithm>
#include <string>
#include <vector>

#include "flowobs/flow.hpp"
#include "flowobs/reconcile.hpp"
#include "render.hpp"

namespace flowobs {

std::string Explanation::render() const {
  std::string out;
  out.reserve(lines.size() * 72u + 64u);
  for (const std::string& line : lines) {
    out.append(line);
    out.push_back('\n');
  }
  return out;
}

namespace detail {

namespace {

const char* on_off(bool value) { return value ? "yes" : "no"; }

void append_kv(std::string& out, const char* key, std::string_view value) {
  out.append("  ");
  out.append(key);
  out.append(" = ");
  out.append(value);
  out.push_back('\n');
}

void append_kv(std::string& out, const char* key, std::uint64_t value) {
  out.append("  ");
  out.append(key);
  out.append(" = ");
  out.append(std::to_string(value));
  out.push_back('\n');
}

void append_kv(std::string& out, const char* key, std::int64_t value) {
  out.append("  ");
  out.append(key);
  out.append(" = ");
  out.append(std::to_string(value));
  out.push_back('\n');
}

std::string consumption_line(const Consumption& consumption) {
  std::string out;
  out.append("bytes=");
  out.append(consumption.bytes_unsupported ? "-" : std::to_string(consumption.bytes));
  out.append(" packets=");
  out.append(consumption.packets_unsupported ? "-" : std::to_string(consumption.packets));
  out.append(" retransmissions=");
  out.append(consumption.retransmissions_unsupported
                 ? "-"
                 : std::to_string(consumption.retransmissions));
  out.append(" unknown_intervals=");
  out.append(std::to_string(consumption.unknown_intervals));
  out.append(consumption.is_exact() ? " exact=yes" : " exact=no");
  return out;
}

}  // namespace

Explanation build_explanation(const FlowSnapshot& snapshot, Timestamp evaluated_at,
                              bool journal_open, RuntimeEpoch current_epoch) {
  Explanation explanation;
  explanation.flow = snapshot.id;
  explanation.canonical_key = snapshot.canonical_key;
  explanation.evaluated_at = evaluated_at;
  std::vector<std::string>& lines = explanation.lines;

  lines.push_back("flow " + flowobs::to_string(snapshot.id) + " key=\"" +
                  snapshot.canonical_key + "\"");
  lines.push_back(std::string("state: ") + std::string(flowobs::to_string(snapshot.state)) +
                  " (cause " + std::string(flowobs::to_string(snapshot.cause)) + ")");
  {
    std::string body;
    append_kv(body, "evaluated_at", evaluated_at.nanos());
    append_kv(body, "evaluated_at_utc", format_timestamp(evaluated_at));
    append_kv(body, "freshness", flowobs::to_string(snapshot.freshness));
    append_kv(body, "consistency", flowobs::to_string(snapshot.consistency));
    append_kv(body, "visibility", flowobs::to_string(snapshot.visibility));
    append_kv(body, "asserts_liveness", on_off(asserts_liveness(snapshot.state)));
    append_kv(body, "asserts_completion", on_off(asserts_completion(snapshot.state)));
    append_kv(body, "terminal", on_off(is_terminal(snapshot.state)));
    append_kv(body, "current_generation", snapshot.current_generation.value());
    append_kv(body, "revision", snapshot.revision.value());
    lines.push_back("  --- classification ---");
    lines.push_back(body);
  }
  {
    std::string body;
    append_kv(body, "created_at", snapshot.created_at.nanos());
    append_kv(body, "state_changed_at", snapshot.state_changed_at.nanos());
    append_kv(body, "first_observed_at", snapshot.first_observed_at.nanos());
    append_kv(body, "last_observed_at", snapshot.last_observed_at.nanos());
    append_kv(body, "last_received_at", snapshot.last_received_at.nanos());
    append_kv(body, "last_progress_at", snapshot.last_progress_at.nanos());
    lines.push_back("  --- instants ---");
    lines.push_back(body);
  }
  {
    std::string body;
    append_kv(body, "endpoint_a", flowobs::to_string(snapshot.endpoint_a));
    append_kv(body, "endpoint_b", flowobs::to_string(snapshot.endpoint_b));
    append_kv(body, "forward", snapshot.coverage.forward_complete
                                   ? "complete"
                                   : (snapshot.coverage.forward_partial ? "partial"
                                                                        : "unobserved"));
    append_kv(body, "reverse", snapshot.coverage.reverse_complete
                                   ? "complete"
                                   : (snapshot.coverage.reverse_partial ? "partial"
                                                                        : "unobserved"));
    append_kv(body, "bidirectional_proven", on_off(snapshot.coverage.bidirectional()));
    append_kv(body, "total", consumption_line(snapshot.total));
    lines.push_back("  --- coverage ---");
    lines.push_back(body);
  }
  {
    std::string body;
    append_kv(body, "restored_from_journal", on_off(snapshot.restored_from_journal));
    append_kv(body, "restored_epoch", snapshot.restored_epoch.value());
    append_kv(body, "current_epoch", snapshot.current_epoch.value());
    append_kv(body, "engine_epoch", current_epoch.value());
    append_kv(body, "journal_open", on_off(journal_open));
    if (snapshot.restored_from_journal &&
        snapshot.restored_epoch != snapshot.current_epoch) {
      append_kv(body, "note",
                "restored evidence is not current; liveness requires fresh evidence in the "
                "current runtime epoch");
    }
    lines.push_back("  --- runtime epoch ---");
    lines.push_back(body);
  }

  lines.push_back("  --- generations ---");
  for (const GenerationView& generation : snapshot.generations) {
    std::string header = "  generation ";
    header.append(std::to_string(generation.generation.generation().value()));
    header.append(generation.is_current ? " (current)" : " (superseded)");
    header.append(" state=");
    header.append(flowobs::to_string(generation.state));
    header.append(" freshness=");
    header.append(flowobs::to_string(generation.freshness));
    header.append(" visibility=");
    header.append(flowobs::to_string(generation.visibility));
    header.append(" consistency=");
    header.append(flowobs::to_string(generation.consistency));
    lines.push_back(header);

    std::string body;
    append_kv(body, "total", consumption_line(generation.total));
    append_kv(body, "forward", consumption_line(generation.forward));
    append_kv(body, "reverse", consumption_line(generation.reverse));
    append_kv(body, "aggregate", consumption_line(generation.aggregate));
    append_kv(body, "first_observed_at", generation.first_observed_at.nanos());
    append_kv(body, "last_observed_at", generation.last_observed_at.nanos());
    append_kv(body, "sources", static_cast<std::uint64_t>(generation.sources.size()));
    append_kv(body, "excluded_stale_sources",
              static_cast<std::uint64_t>(generation.excluded_stale_sources));
    append_kv(body, "path_correlation", on_off(generation.has_path_correlation));
    append_kv(body, "queue_correlation", on_off(generation.has_queue_correlation));
    if (generation.path.has_value()) {
      append_kv(body, "path", flowobs::to_string(*generation.path));
    }
    if (generation.unresolved_conflict) {
      append_kv(body, "conflict", generation.conflict_detail);
    }
    lines.push_back(body);

    for (const SourceView& source : generation.sources) {
      std::string source_header = "    source ";
      source_header.append(flowobs::to_string(source.source));
      source_header.append(" key=\"");
      source_header.append(source.canonical_key);
      source_header.append("\" authority=");
      source_header.append(std::to_string(source.authority));
      source_header.append(" freshness=");
      source_header.append(flowobs::to_string(source.freshness));
      source_header.append(" usable=");
      source_header.append(on_off(source.usable_for_liveness()));
      lines.push_back(source_header);
      std::string source_body;
      append_kv(source_body, "capabilities", source.capabilities.describe());
      append_kv(source_body, "incarnation", source.incarnation.value());
      append_kv(source_body, "epoch", source.epoch.value());
      append_kv(source_body, "high_revision", source.high_revision.value());
      append_kv(source_body, "high_sequence", source.high_sequence.value());
      append_kv(source_body, "last_kind", flowobs::to_string(source.last_kind));
      append_kv(source_body, "last_evidence", flowobs::to_string(source.last_evidence));
      append_kv(source_body, "accepted", static_cast<std::uint64_t>(source.accepted_observations));
      append_kv(source_body, "duplicates", static_cast<std::uint64_t>(source.duplicates_suppressed));
      append_kv(source_body, "content_conflicts",
                static_cast<std::uint64_t>(source.content_conflicts));
      append_kv(source_body, "dropped", static_cast<std::uint64_t>(source.dropped_observations));
      append_kv(source_body, "regressions", static_cast<std::uint64_t>(source.regressions));
      append_kv(source_body, "restart_gaps", static_cast<std::uint64_t>(source.restart_gaps));
      append_kv(source_body, "forward", consumption_line(source.forward_totals));
      append_kv(source_body, "reverse", consumption_line(source.reverse_totals));
      append_kv(source_body, "aggregate", consumption_line(source.aggregate_totals));
      if (source.declared_terminal.has_value()) {
        append_kv(source_body, "declared_terminal",
                  flowobs::to_string(*source.declared_terminal));
      }
      append_kv(source_body, "reconfirmed", on_off(source.reconfirmed));
      lines.push_back(source_body);
    }
  }

  lines.push_back("  --- anomalies ---");
  if (snapshot.anomalies.empty() && snapshot.anomalies_dropped == 0) {
    lines.push_back("  none");
  }
  for (const Anomaly& anomaly : snapshot.anomalies) {
    std::string line = "  ";
    line.append(flowobs::to_string(anomaly.kind));
    line.append(anomaly_is_conservative_refusal(anomaly.kind) ? " [conservative refusal]"
                                                              : " [informational]");
    line.append(" at=");
    line.append(std::to_string(anomaly.at.nanos()));
    if (anomaly.source.valid()) {
      line.append(" source=");
      line.append(flowobs::to_string(anomaly.source));
    }
    if (!anomaly.detail.empty()) {
      line.append(" ");
      line.append(anomaly.detail);
    }
    lines.push_back(line);
  }
  if (snapshot.anomalies_dropped > 0) {
    lines.push_back("  anomalies_dropped = " + std::to_string(snapshot.anomalies_dropped));
  }

  lines.push_back("  --- why this state ---");
  if (snapshot.state == FlowState::Unknown) {
    lines.push_back(
        "  no accepted evidence names this flow; Unknown is the honest answer, not a default");
  } else if (snapshot.state == FlowState::Observed) {
    lines.push_back(
        "  evidence exists but carries no current progress; existence is not liveness");
  } else if (snapshot.state == FlowState::Active) {
    lines.push_back("  fresh evidence inside the idle window shows progress");
  } else if (snapshot.state == FlowState::Idle) {
    lines.push_back("  no progress within expiry.idle_after; idle is not completion");
  } else if (snapshot.state == FlowState::Completed) {
    lines.push_back(
        "  completion was declared by a source holding the completion capability");
  } else if (snapshot.state == FlowState::Reset) {
    lines.push_back(
        "  a reset was declared; the consumption across the reset boundary is unknown and is "
        "reported as such");
  } else if (snapshot.state == FlowState::Expired) {
    lines.push_back(
        "  the expiry policy closed this generation; expired is not completed and the runtime "
        "does not claim the flow finished");
  } else if (snapshot.state == FlowState::Conflicting) {
    lines.push_back("  equal-authority sources disagree and no rule resolves it");
  }
  if (snapshot.visibility == Visibility::OneSided) {
    lines.push_back(
        "  one-sided visibility: the reverse direction was never observed, so bidirectional "
        "behaviour is unproven");
  } else if (snapshot.visibility == Visibility::Unknown) {
    lines.push_back("  no accepted observation currently proves coverage of either direction");
  }
  if (snapshot.freshness != Freshness::Fresh) {
    lines.push_back(std::string("  freshness is ") +
                    std::string(flowobs::to_string(snapshot.freshness)) +
                    ", so this evidence cannot support current attribution");
  }
  return explanation;
}

}  // namespace detail
}  // namespace flowobs
