// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef FLOWOBS_ERROR_HPP
#define FLOWOBS_ERROR_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "flowobs/export.hpp"

namespace flowobs {

// Every failure the runtime can report is classified. Callers are expected to
// branch on the code, never on the message text. Messages are for humans and
// are always bounded by Limits::max_detail_bytes.
enum class ErrorCode : std::uint16_t {
  Ok = 0,

  // Caller error: the request itself was malformed or out of contract.
  InvalidArgument = 1,
  OutOfRange = 2,
  LimitExceeded = 3,

  // Lookup failure: the object is valid but unknown to this runtime.
  NotFound = 4,

  // Evidence was refused because it cannot be trusted for the current view.
  StaleEvidence = 10,
  StaleEpoch = 11,
  StaleIncarnation = 12,
  StaleGeneration = 13,
  StaleRevision = 14,
  SequenceReplay = 15,
  DuplicateEvidence = 16,
  EvidenceConflict = 17,
  UnsupportedCapability = 18,
  IncompleteEvidence = 19,

  // Persistence.
  IntegrityFailure = 30,
  FormatMismatch = 31,
  VersionMismatch = 32,
  TruncatedRecord = 33,

  // Lifecycle of the runtime itself.
  NotOpen = 40,
  AlreadyOpen = 41,
  ShuttingDown = 42,
  Cancelled = 43,
  AlreadyRunning = 44,
  NotRunning = 45,

  // Transport.
  ProtocolError = 60,
  ConnectionClosed = 61,
  ConnectionRefused = 62,
  AddressInUse = 63,

  // Arithmetic and allocation.
  ArithmeticOverflow = 70,
  OutOfMemory = 71,

  // Filesystem / OS.
  IoError = 80,

  // A defensive catch-all. Never returned for a condition that has a more
  // specific code.
  Internal = 90,
};

FLOWOBS_PUBLIC std::string_view to_string(ErrorCode code) noexcept;

// True for codes that mean "the evidence was seen and deliberately not used".
FLOWOBS_PUBLIC bool is_evidence_refusal(ErrorCode code) noexcept;

// True for codes that indicate the runtime detected an integrity problem.
FLOWOBS_PUBLIC bool is_integrity_failure(ErrorCode code) noexcept;

class FLOWOBS_PUBLIC Status {
 public:
  Status() noexcept = default;
  Status(ErrorCode code, std::string message) noexcept
      : code_(code), message_(std::move(message)) {}

  [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::Ok; }
  [[nodiscard]] bool failed() const noexcept { return code_ != ErrorCode::Ok; }
  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }
  [[nodiscard]] std::string_view code_name() const noexcept { return to_string(code_); }

  // "invalid_argument: flow id is zero" - stable and log friendly.
  [[nodiscard]] std::string describe() const;

  void clear() noexcept {
    code_ = ErrorCode::Ok;
    message_.clear();
  }

 private:
  ErrorCode code_ = ErrorCode::Ok;
  std::string message_;
};

// Value-or-Status. Library code never throws for expected failures; the
// runtime only ever propagates std::bad_alloc.
template <class T>
class Result {
 public:
  Result(T value) noexcept(std::is_nothrow_move_constructible_v<T>)
      : value_(std::move(value)), status_() {}

  // A Result built from a success Status is a programming error and is turned
  // into Internal so that it can never be mistaken for success.
  Result(Status status) noexcept : value_(), status_(std::move(status)) {
    if (status_.ok()) {
      status_ = Status(ErrorCode::Internal, "Result constructed from a success status");
    }
  }

  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }
  [[nodiscard]] ErrorCode code() const noexcept { return status_.code(); }
  [[nodiscard]] const std::string& message() const noexcept { return status_.message(); }

  [[nodiscard]] T& value() & noexcept { return *value_; }
  [[nodiscard]] const T& value() const& noexcept { return *value_; }
  [[nodiscard]] T&& value() && noexcept { return std::move(*value_); }

  T value_or(T fallback) const {
    return value_.has_value() ? *value_ : std::move(fallback);
  }

  [[nodiscard]] const T* operator->() const noexcept { return &*value_; }
  [[nodiscard]] T* operator->() noexcept { return &*value_; }
  [[nodiscard]] const T& operator*() const& noexcept { return *value_; }
  [[nodiscard]] T& operator*() & noexcept { return *value_; }

 private:
  std::optional<T> value_;
  Status status_;
};

// Narrow helpers used throughout the implementation.
inline Status ok_status() noexcept { return Status(); }

// A Status that never conflicts with Status::ok(), the predicate.
inline Status make_ok() noexcept { return Status(); }

inline Status make_error(ErrorCode code, std::string message) {
  return Status(code, std::move(message));
}

inline Status invalid_argument(std::string message) {
  return Status(ErrorCode::InvalidArgument, std::move(message));
}

inline Status limit_exceeded(std::string message) {
  return Status(ErrorCode::LimitExceeded, std::move(message));
}

inline Status out_of_range_error(std::string message) {
  return Status(ErrorCode::OutOfRange, std::move(message));
}

inline Status not_open(std::string message) {
  return Status(ErrorCode::NotOpen, std::move(message));
}

inline Status shutting_down(std::string message) {
  return Status(ErrorCode::ShuttingDown, std::move(message));
}

inline Status not_found(std::string message) {
  return Status(ErrorCode::NotFound, std::move(message));
}

inline Status internal_error(std::string message) {
  return Status(ErrorCode::Internal, std::move(message));
}

}  // namespace flowobs

#endif  // FLOWOBS_ERROR_HPP
