// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "lockorder.hpp"

#include <atomic>

namespace flowobs {
namespace detail {

thread_local std::int32_t LockAudit::depth_ = 0;
thread_local std::int32_t LockAudit::stack_[16] = {};

namespace {
std::atomic<std::uint64_t> g_violations{0};
std::atomic<bool> g_enabled{true};
}  // namespace

const char* lock_rank_name(LockRank rank) noexcept {
  switch (rank) {
    case LockRank::None: return "none";
    case LockRank::Engine: return "engine";
    case LockRank::Queue: return "queue";
    case LockRank::Journal: return "journal";
  }
  return "invalid";
}

bool LockAudit::enter(LockRank rank, const char** violation) noexcept {
  if (!g_enabled.load(std::memory_order_relaxed)) {
    return true;
  }
  const std::int32_t raw = static_cast<std::int32_t>(rank);
  if (depth_ > 0 && stack_[depth_ - 1] >= raw) {
    if (violation != nullptr) {
      *violation = "lock order violation";
    }
    g_violations.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (depth_ < 16) {
    stack_[depth_] = raw;
  }
  ++depth_;
  return true;
}

void LockAudit::leave(LockRank rank) noexcept {
  if (!g_enabled.load(std::memory_order_relaxed)) {
    return;
  }
  (void)rank;
  if (depth_ > 0) {
    --depth_;
    if (depth_ < 16) {
      stack_[depth_] = 0;
    }
  }
}

std::uint64_t LockAudit::violations() noexcept {
  return g_violations.load(std::memory_order_relaxed);
}

void LockAudit::reset_violations() noexcept {
  g_violations.store(0, std::memory_order_relaxed);
}

std::int32_t LockAudit::depth() noexcept { return depth_; }

bool LockAudit::enabled() noexcept { return g_enabled.load(std::memory_order_relaxed); }

void LockAudit::set_enabled(bool enabled) noexcept {
  g_enabled.store(enabled, std::memory_order_relaxed);
}

}  // namespace detail
}  // namespace flowobs
