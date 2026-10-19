#pragma once

#include "hid_keyboard.h"

namespace esphome::kvm_usb_bridge {

// A key transition is useful only near the time it happened. Never replay a
// typing backlog after the KVM wakes or starts polling again.
constexpr uint32_t MAX_REPORT_AGE_MS = 100;

class ReportBuffer {
 public:
  static constexpr size_t CAPACITY = 64;
  void clear() { head_ = count_ = 0; }
  void release(uint32_t now) { clear(); push(BootReport{}, now); }
  bool push(const BootReport &report, uint32_t now) {
    if (count_ == CAPACITY) {
      release(now); // Release-only recovery, not a replay of the latest held key.
      return false;
    }
    entries_[(head_ + count_) % CAPACITY] = {report, now};
    count_++;
    return true;
  }
  const BootReport &front() const { return entries_[head_].report; }
  void pop() {
    if (count_ != 0) { head_ = (head_ + 1) % CAPACITY; count_--; }
  }
  bool empty() const { return count_ == 0; }
  size_t size() const { return count_; }
  uint32_t age(uint32_t now) const { return empty() ? 0 : uint32_t(now - entries_[head_].at); }
  bool stale(uint32_t now) const { return !empty() && age(now) > MAX_REPORT_AGE_MS; }

 private:
  struct Entry { BootReport report; uint32_t at{0}; };
  std::array<Entry, CAPACITY> entries_{};
  size_t head_{0}, count_{0};
};

class ReleaseGate {
 public:
  void arm() { waiting_ = true; }
  bool waiting() const { return waiting_; }
  void observe(const KeyState &state, bool fresh_physical_report) {
    if (!waiting_ || !fresh_physical_report) return;
    for (auto bits : state.bits) if (bits != 0) return;
    waiting_ = false;
  }
 private:
  bool waiting_{false};
};

// uint32_t clock wrap is expected; a recovery boundary is never more than
// half the clock range away from an event still in the small input queue.
inline bool report_is_fresh(uint32_t now, uint32_t at, bool has_boundary, uint32_t boundary) {
  return uint32_t(now - at) <= MAX_REPORT_AGE_MS &&
         (!has_boundary || int32_t(at - boundary) >= 0);
}

} // namespace esphome::kvm_usb_bridge
