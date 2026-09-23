// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The runtime never reads a clock implicitly on the ingest path. It asks an
// injected Clock for "now" when it must evaluate freshness or expire evidence.
// Tests inject ManualClock so that every freshness decision is reproducible.

#ifndef FLOWOBS_CLOCK_HPP
#define FLOWOBS_CLOCK_HPP

#include <atomic>
#include <memory>

#include "flowobs/export.hpp"
#include "flowobs/time.hpp"

namespace flowobs {

class FLOWOBS_PUBLIC Clock {
 public:
  Clock() = default;
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
  virtual ~Clock();

  // Wall-clock instant used for freshness. Monotonicity is not guaranteed by
  // this interface; the runtime defends against backwards jumps by clamping.
  [[nodiscard]] virtual Timestamp now() const = 0;

  // A monotonically non-decreasing reading, used only for internal timing such
  // as measuring durations in benchmarks and timeouts on sockets.
  [[nodiscard]] virtual std::int64_t monotonic_nanos() const = 0;

  [[nodiscard]] virtual const char* name() const noexcept = 0;
};

// System wall clock. This is the only clock the runtime uses unless a caller
// injects another one.
class FLOWOBS_PUBLIC SystemClock final : public Clock {
 public:
  SystemClock() = default;
  [[nodiscard]] Timestamp now() const override;
  [[nodiscard]] std::int64_t monotonic_nanos() const override;
  [[nodiscard]] const char* name() const noexcept override { return "system"; }

  [[nodiscard]] static const SystemClock& instance() noexcept;
};

// Deterministic clock for tests and replay. Thread safe.
class FLOWOBS_PUBLIC ManualClock final : public Clock {
 public:
  explicit ManualClock(Timestamp start = Timestamp::from_nanos(1700000000000000000LL)) noexcept
      : now_(start.nanos()), monotonic_(0) {}

  [[nodiscard]] Timestamp now() const override {
    return Timestamp::from_nanos(now_.load(std::memory_order_relaxed));
  }
  [[nodiscard]] std::int64_t monotonic_nanos() const override {
    return monotonic_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] const char* name() const noexcept override { return "manual"; }

  void set(Timestamp ts) noexcept { now_.store(ts.nanos(), std::memory_order_relaxed); }
  void advance(Duration d) noexcept {
    now_.store(now_.load(std::memory_order_relaxed) + d.nanos(), std::memory_order_relaxed);
    monotonic_.store(monotonic_.load(std::memory_order_relaxed) + d.nanos(),
                     std::memory_order_relaxed);
  }
  void set_monotonic(std::int64_t nanos) noexcept {
    monotonic_.store(nanos, std::memory_order_relaxed);
  }

 private:
  std::atomic<std::int64_t> now_;
  std::atomic<std::int64_t> monotonic_;
};

}  // namespace flowobs

#endif  // FLOWOBS_CLOCK_HPP
