// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "flowobs/error.hpp"

namespace flowobs {

std::string_view to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok: return "ok";
    case ErrorCode::InvalidArgument: return "invalid_argument";
    case ErrorCode::OutOfRange: return "out_of_range";
    case ErrorCode::LimitExceeded: return "limit_exceeded";
    case ErrorCode::NotFound: return "not_found";
    case ErrorCode::StaleEvidence: return "stale_evidence";
    case ErrorCode::StaleEpoch: return "stale_epoch";
    case ErrorCode::StaleIncarnation: return "stale_incarnation";
    case ErrorCode::StaleGeneration: return "stale_generation";
    case ErrorCode::StaleRevision: return "stale_revision";
    case ErrorCode::SequenceReplay: return "sequence_replay";
    case ErrorCode::DuplicateEvidence: return "duplicate_evidence";
    case ErrorCode::EvidenceConflict: return "evidence_conflict";
    case ErrorCode::UnsupportedCapability: return "unsupported_capability";
    case ErrorCode::IncompleteEvidence: return "incomplete_evidence";
    case ErrorCode::IntegrityFailure: return "integrity_failure";
    case ErrorCode::FormatMismatch: return "format_mismatch";
    case ErrorCode::VersionMismatch: return "version_mismatch";
    case ErrorCode::TruncatedRecord: return "truncated_record";
    case ErrorCode::NotOpen: return "not_open";
    case ErrorCode::AlreadyOpen: return "already_open";
    case ErrorCode::ShuttingDown: return "shutting_down";
    case ErrorCode::Cancelled: return "cancelled";
    case ErrorCode::AlreadyRunning: return "already_running";
    case ErrorCode::NotRunning: return "not_running";
    case ErrorCode::ProtocolError: return "protocol_error";
    case ErrorCode::ConnectionClosed: return "connection_closed";
    case ErrorCode::ConnectionRefused: return "connection_refused";
    case ErrorCode::AddressInUse: return "address_in_use";
    case ErrorCode::ArithmeticOverflow: return "arithmetic_overflow";
    case ErrorCode::OutOfMemory: return "out_of_memory";
    case ErrorCode::IoError: return "io_error";
    case ErrorCode::Internal: return "internal";
  }
  return "unknown";
}

bool is_evidence_refusal(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::StaleEvidence:
    case ErrorCode::StaleEpoch:
    case ErrorCode::StaleIncarnation:
    case ErrorCode::StaleGeneration:
    case ErrorCode::StaleRevision:
    case ErrorCode::SequenceReplay:
    case ErrorCode::DuplicateEvidence:
    case ErrorCode::EvidenceConflict:
    case ErrorCode::UnsupportedCapability:
    case ErrorCode::IncompleteEvidence:
      return true;
    default:
      return false;
  }
}

bool is_integrity_failure(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::IntegrityFailure:
    case ErrorCode::FormatMismatch:
    case ErrorCode::VersionMismatch:
    case ErrorCode::TruncatedRecord:
      return true;
    default:
      return false;
  }
}

std::string Status::describe() const {
  std::string out;
  out.reserve(message_.size() + 24u);
  out.append(to_string(code_));
  if (!message_.empty()) {
    out.append(": ");
    out.append(message_);
  }
  return out;
}

}  // namespace flowobs
