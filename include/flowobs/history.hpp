// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Bounded lifecycle history. History is a first-class, bounded resource: it is
// retained for a bounded number of flows and a bounded number of transitions
// per flow, and every drop is counted.

#ifndef FLOWOBS_HISTORY_HPP
#define FLOWOBS_HISTORY_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "flowobs/export.hpp"
#include "flowobs/flow.hpp"
#include "flowobs/identity.hpp"

namespace flowobs {

// Fixed-capacity ring buffer of lifecycle events for one flow. Push overwrites
// the oldest entry and counts the overwrite; nothing is silently lost.
class FLOWOBS_PUBLIC LifecycleHistory final {
 public:
  LifecycleHistory() = default;
  explicit LifecycleHistory(std::size_t capacity);

  void set_capacity(std::size_t capacity);

  void push(const LifecycleEvent& event);

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

  // Events from oldest to newest.
  [[nodiscard]] std::vector<LifecycleEvent> to_vector() const;
  // The most recent `limit` events, oldest first.
  [[nodiscard]] std::vector<LifecycleEvent> tail(std::size_t limit) const;

  void clear() noexcept;

 private:
  std::vector<LifecycleEvent> buffer_;
  std::size_t head_ = 0;   // index of the oldest element
  std::size_t size_ = 0;
  std::size_t capacity_ = 0;
  std::uint64_t dropped_ = 0;
};

}  // namespace flowobs

#endif  // FLOWOBS_HISTORY_HPP
