// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "flowobs/policy.hpp"

#include <string>

namespace flowobs {

namespace {
Status require(bool condition, const char* message) {
  return condition ? make_ok() : Status(ErrorCode::OutOfRange, message);
}
}  // namespace

Status FreshnessPolicy::validate() const {
  Status s = require(fresh_window.nanos() >= 0, "freshness.fresh_window must not be negative");
  if (s.failed()) return s;
  s = require(expire_window.nanos() > 0, "freshness.expire_window must be positive");
  if (s.failed()) return s;
  s = require(expire_window.nanos() >= fresh_window.nanos(),
              "freshness.expire_window must be at least freshness.fresh_window");
  if (s.failed()) return s;
  return make_ok();
}

std::string FreshnessPolicy::describe() const {
  std::string out;
  out.reserve(192);
  out.append("fresh_window_ns=");
  out.append(std::to_string(fresh_window.nanos()));
  out.append(" expire_window_ns=");
  out.append(std::to_string(expire_window.nanos()));
  out.append(" clamp_future_timestamps=");
  out.append(clamp_future_timestamps ? "true" : "false");
  out.append(" require_reconfirmation_after_restart=");
  out.append(require_reconfirmation_after_restart ? "true" : "false");
  return out;
}

Status ExpiryPolicy::validate() const {
  Status s = require(idle_after.nanos() >= 0, "expiry.idle_after must not be negative");
  if (s.failed()) return s;
  s = require(expire_after.nanos() > 0, "expiry.expire_after must be positive");
  if (s.failed()) return s;
  s = require(expire_after.nanos() >= idle_after.nanos(),
              "expiry.expire_after must be at least expiry.idle_after");
  if (s.failed()) return s;
  return make_ok();
}

std::string ExpiryPolicy::describe() const {
  std::string out;
  out.reserve(128);
  out.append("enabled=");
  out.append(enabled ? "true" : "false");
  out.append(" idle_after_ns=");
  out.append(std::to_string(idle_after.nanos()));
  out.append(" expire_after_ns=");
  out.append(std::to_string(expire_after.nanos()));
  return out;
}

Status CounterPolicy::validate() const { return make_ok(); }

std::string CounterPolicy::describe() const {
  std::string out;
  out.reserve(96);
  out.append("treat_regression_as_reset=");
  out.append(treat_regression_as_reset ? "true" : "false");
  out.append(" honor_explicit_counter_reset=");
  out.append(honor_explicit_counter_reset ? "true" : "false");
  return out;
}

Status ReconciliationPolicy::validate() const {
  Status s = require(relative_tolerance_ppm <= 1000000ull,
                     "reconciliation.relative_tolerance_ppm must not exceed 1000000");
  if (s.failed()) return s;
  switch (method) {
    case Method::PrimaryAuthoritative:
    case Method::MaximumObserved:
    case Method::ConservativeMinimum:
      return make_ok();
  }
  return Status(ErrorCode::InvalidArgument, "reconciliation.method is not a known method");
}

std::string ReconciliationPolicy::describe() const {
  std::string out;
  out.reserve(192);
  out.append("method=");
  switch (method) {
    case Method::PrimaryAuthoritative: out.append("primary-authoritative"); break;
    case Method::MaximumObserved: out.append("maximum-observed"); break;
    case Method::ConservativeMinimum: out.append("conservative-minimum"); break;
    default: out.append("invalid"); break;
  }
  out.append(" absolute_tolerance_bytes=");
  out.append(std::to_string(absolute_tolerance_bytes));
  out.append(" relative_tolerance_ppm=");
  out.append(std::to_string(relative_tolerance_ppm));
  out.append(" higher_authority_resolves=");
  out.append(higher_authority_resolves ? "true" : "false");
  out.append(" exclude_stale_from_totals=");
  out.append(exclude_stale_from_totals ? "true" : "false");
  return out;
}

Status AttributionPolicy::validate() const {
  switch (method) {
    case Method::EndpointOnly:
    case Method::UniformAcrossHops:
    case Method::ProportionalToCapacity:
      return make_ok();
  }
  return Status(ErrorCode::InvalidArgument, "attribution.method is not a known method");
}

std::string AttributionPolicy::describe() const {
  std::string out;
  out.reserve(160);
  out.append("method=");
  switch (method) {
    case Method::EndpointOnly: out.append("endpoint-only"); break;
    case Method::UniformAcrossHops: out.append("uniform-across-hops"); break;
    case Method::ProportionalToCapacity: out.append("proportional-to-capacity"); break;
    default: out.append("invalid"); break;
  }
  out.append(" require_fresh_generation=");
  out.append(require_fresh_generation ? "true" : "false");
  out.append(" require_current_generation=");
  out.append(require_current_generation ? "true" : "false");
  return out;
}

Status RecoveryPolicy::validate() const {
  switch (torn_tail) {
    case TornTail::Reject:
    case TornTail::TruncateTornTail:
      return make_ok();
  }
  return Status(ErrorCode::InvalidArgument, "recovery.torn_tail is not a known policy");
}

std::string RecoveryPolicy::describe() const {
  std::string out;
  out.reserve(224);
  out.append("torn_tail=");
  out.append(torn_tail == TornTail::Reject ? "reject" : "truncate-torn-tail");
  out.append(" enforce_format_version=");
  out.append(enforce_format_version ? "true" : "false");
  out.append(" enforce_identity_scheme=");
  out.append(enforce_identity_scheme ? "true" : "false");
  out.append(" demote_restored_liveness=");
  out.append(demote_restored_liveness ? "true" : "false");
  out.append(" new_accounting_interval_after_restart=");
  out.append(new_accounting_interval_after_restart ? "true" : "false");
  return out;
}

Status QueryPolicy::validate() const {
  Status s = require(max_rows > 0, "query.max_rows must be greater than zero");
  if (s.failed()) return s;
  s = require(max_window.nanos() > 0, "query.max_window must be positive");
  if (s.failed()) return s;
  return make_ok();
}

std::string QueryPolicy::describe() const {
  std::string out;
  out.reserve(128);
  out.append("max_rows=");
  out.append(std::to_string(max_rows));
  out.append(" max_window_ns=");
  out.append(std::to_string(max_window.nanos()));
  out.append(" include_anomalies=");
  out.append(include_anomalies ? "true" : "false");
  out.append(" include_sources=");
  out.append(include_sources ? "true" : "false");
  out.append(" include_generations=");
  out.append(include_generations ? "true" : "false");
  return out;
}

}  // namespace flowobs
