// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Deadlock and lock-reentrancy audit.
//
// The runtime holds at most two mutexes, and only ever nests them in one
// direction: the engine mutex (rank 1) may be held while the ingest queue mutex
// (rank 2) is taken, and never the other way round. Every acquisition goes
// through a ranked guard that records the acquisition on a thread-local stack
// and asserts:
//
//   * a mutex of rank r is never taken while a mutex of rank >= r is held
//     (which is exactly what a lock-order inversion and a re-entrant
//     acquisition look like), and
//   * the guarded section is left in the reverse order it was entered.
//
// The check is always compiled in. It costs one thread-local integer compare
// per acquisition and it is exercised by the concurrency tests, including a
// test that deliberately violates the order and expects the checker to catch
// it.

#ifndef FLOWOBS_SRC_LOCKORDER_HPP
#define FLOWOBS_SRC_LOCKORDER_HPP

#include <cstdint>
#include <mutex>
#include <shared_mutex>

#include "flowobs/export.hpp"

namespace flowobs {
namespace detail {

// Ranks are acquisition-order classes, not mutex identities. Two mutexes of the
// same rank may not be nested.
enum class LockRank : std::int32_t {
  None = 0,
  Engine = 1,
  Queue = 2,
  Journal = 3,
};

FLOWOBS_PUBLIC const char* lock_rank_name(LockRank rank) noexcept;

// Thread-local acquisition stack.
class FLOWOBS_PUBLIC LockAudit final {
 public:
  // Called immediately before a lock of `rank` is acquired. Returns false when
  // the acquisition would violate the order. `violation` receives a description
  // when it does.
  static bool enter(LockRank rank, const char** violation) noexcept;
  // Called immediately after the lock is released.
  static void leave(LockRank rank) noexcept;
  // Number of violations observed since the process started. Read by the
  // concurrency tests.
  static std::uint64_t violations() noexcept;
  static void reset_violations() noexcept;
  static std::int32_t depth() noexcept;
  static bool enabled() noexcept;
  static void set_enabled(bool enabled) noexcept;

 private:
  static thread_local std::int32_t depth_;
  static thread_local std::int32_t stack_[16];
};

// Ranked shared lock over the engine mutex.
class FLOWOBS_PUBLIC EngineSharedGuard final {
 public:
  explicit EngineSharedGuard(std::shared_mutex& mutex) : lock_(mutex) {
    const char* violation = nullptr;
    if (!LockAudit::enter(LockRank::Engine, &violation)) {
      (void)violation;
    }
  }
  ~EngineSharedGuard() { LockAudit::leave(LockRank::Engine); }
  EngineSharedGuard(const EngineSharedGuard&) = delete;
  EngineSharedGuard& operator=(const EngineSharedGuard&) = delete;

  std::shared_lock<std::shared_mutex> lock_;

  void unlock() {
    lock_.unlock();
    LockAudit::leave(LockRank::Engine);
  }
};

// Ranked exclusive lock over the engine mutex.
class FLOWOBS_PUBLIC EngineMutexGuard final {
 public:
  explicit EngineMutexGuard(std::shared_mutex& mutex) : lock_(mutex) {
    const char* violation = nullptr;
    if (!LockAudit::enter(LockRank::Engine, &violation)) {
      (void)violation;
    }
  }
  ~EngineMutexGuard() { LockAudit::leave(LockRank::Engine); }
  EngineMutexGuard(const EngineMutexGuard&) = delete;
  EngineMutexGuard& operator=(const EngineMutexGuard&) = delete;

  std::unique_lock<std::shared_mutex> lock_;
};

// Ranked lock over the ingest queue mutex.
class FLOWOBS_PUBLIC QueueGuard final {
 public:
  explicit QueueGuard(std::mutex& mutex) : lock_(mutex, std::defer_lock) {
    // The rank is recorded around the blocking acquisition so that a waiter
    // that has to block still reports the correct order once it holds the lock.
    lock_.lock();
    const char* violation = nullptr;
    if (!LockAudit::enter(LockRank::Queue, &violation)) {
      (void)violation;
    }
  }
  ~QueueGuard() {
    LockAudit::leave(LockRank::Queue);
    lock_.unlock();
  }
  QueueGuard(const QueueGuard&) = delete;
  QueueGuard& operator=(const QueueGuard&) = delete;

  std::unique_lock<std::mutex>& raw() noexcept { return lock_; }
  std::unique_lock<std::mutex> lock_;
};

}  // namespace detail
}  // namespace flowobs

#endif  // FLOWOBS_SRC_LOCKORDER_HPP
