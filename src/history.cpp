// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "flowobs/history.hpp"

#include <utility>

namespace flowobs {

LifecycleHistory::LifecycleHistory(std::size_t capacity) { set_capacity(capacity); }

void LifecycleHistory::set_capacity(std::size_t capacity) {
  if (capacity == capacity_ && buffer_.size() == capacity_) {
    return;
  }
  if (capacity == 0) {
    capacity_ = 0;
    buffer_.clear();
    head_ = 0;
    size_ = 0;
    return;
  }
  // Read the existing events in canonical order *before* the capacity changes:
  // the ring indexing depends on capacity_.
  std::vector<LifecycleEvent> ordered = to_vector();
  capacity_ = capacity;
  {
    buffer_.assign(capacity_, LifecycleEvent{});
    const std::size_t keep = ordered.size() < capacity_ ? ordered.size() : capacity_;
    const std::size_t skip = ordered.size() - keep;
    dropped_ += skip;
    for (std::size_t i = 0; i < keep; ++i) {
      buffer_[i] = std::move(ordered[skip + i]);
    }
    head_ = 0;
    size_ = keep;
  }
}

void LifecycleHistory::push(const LifecycleEvent& event) {
  if (capacity_ == 0) {
    ++dropped_;
    return;
  }
  if (size_ < capacity_) {
    buffer_[(head_ + size_) % capacity_] = event;
    ++size_;
    return;
  }
  buffer_[head_] = event;
  head_ = (head_ + 1) % capacity_;
  ++dropped_;
}

std::vector<LifecycleEvent> LifecycleHistory::to_vector() const {
  std::vector<LifecycleEvent> out;
  out.reserve(size_);
  for (std::size_t i = 0; i < size_; ++i) {
    out.push_back(buffer_[(head_ + i) % capacity_]);
  }
  return out;
}

std::vector<LifecycleEvent> LifecycleHistory::tail(std::size_t limit) const {
  if (limit == 0 || size_ == 0) {
    return {};
  }
  const std::size_t take = limit < size_ ? limit : size_;
  std::vector<LifecycleEvent> out;
  out.reserve(take);
  const std::size_t start = size_ - take;
  for (std::size_t i = start; i < size_; ++i) {
    out.push_back(buffer_[(head_ + i) % capacity_]);
  }
  return out;
}

void LifecycleHistory::clear() noexcept {
  // Resets the ring completely, including its capacity, so that a later push
  // cannot index into an empty buffer.
  buffer_.clear();
  head_ = 0;
  size_ = 0;
  capacity_ = 0;
  dropped_ = 0;
}

}  // namespace flowobs
