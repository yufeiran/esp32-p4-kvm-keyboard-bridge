#pragma once

#include "hid_keyboard.h"

namespace esphome::kvm_usb_bridge {

// Transport-independent Ctrl+Alt+/ filter and TESmart next-input sequence.
// Only the USB worker accesses this state; callbacks acknowledge via atomics.
class SwitchHotkey {
 public:
  void set_enabled(bool value) { enabled_ = value; }
  bool enabled() const { return enabled_; }
  bool active() const { return active_; }
  bool suppressing_physical() const { return active_ || suppressed_; }
  bool waiting_for_ack() const { return waiting_for_ack_; }

  bool observe(const KeyState &state, uint32_t now) {
    const bool chord = core_matches_(state);
    const bool chord_edge = chord && !chord_was_down_;
    chord_was_down_ = chord;
    if (suppressed_) {
      if (!active_ && empty_(state)) suppressed_ = false;
      return false;
    }
    if (!enabled_ || !chord_edge || extra_modifiers_(state)) return false;
    // Consume even a debounced repeat; never leak the shortcut to desktop apps.
    suppressed_ = true;
    if (has_triggered_ && uint32_t(now - last_trigger_ms_) < 800) return false;
    has_triggered_ = true;
    last_trigger_ms_ = now;
    started_ms_ = now;
    due_ms_ = now;
    step_ = 0;
    waiting_for_ack_ = false;
    active_ = true;
    return true;
  }

  BootReport filtered_report(const KeyState &state, uint8_t maximum = 0x65) const {
    return suppressed_ || active_ ? BootReport{} : boot_report(state, maximum);
  }

  bool report_due(uint32_t now) const {
    return active_ && !waiting_for_ack_ && int32_t(now - due_ms_) >= 0;
  }
  BootReport next_report() const {
    BootReport report;
    // Release old PC first; tap RightCtrl twice; tap PageDown; finish released.
    if (step_ == 1 || step_ == 3) report.modifiers = 0x10;
    if (step_ == 5) report.keys[0] = 0x4E;
    return report;
  }
  void submitted() { waiting_for_ack_ = true; }
  bool acknowledge(uint32_t now) {
    if (!active_ || !waiting_for_ack_) return false;
    waiting_for_ack_ = false;
    if (++step_ == 7) {
      active_ = false;
      return true;
    }
    // Give the KVM's hotkey decoder time to observe each press and release.
    due_ms_ = now + 30;
    return false;
  }
  bool timed_out(uint32_t now) const { return active_ && uint32_t(now - started_ms_) >= 2000; }
  void abort() { active_ = false; waiting_for_ack_ = false; }

 private:
  static bool empty_(const KeyState &state) {
    for (auto bits : state.bits) if (bits != 0) return false;
    return true;
  }
  static bool core_matches_(const KeyState &state) {
    const bool ctrl = state.down(0xE0) || state.down(0xE4);
    const bool alt = state.down(0xE2) || state.down(0xE6);
    return ctrl && alt && state.down(0x38);
  }
  static bool extra_modifiers_(const KeyState &state) {
    return state.down(0xE1) || state.down(0xE5) || state.down(0xE3) || state.down(0xE7);
  }
  bool enabled_{false};
  bool chord_was_down_{false};
  bool suppressed_{false};
  bool active_{false};
  bool waiting_for_ack_{false};
  bool has_triggered_{false};
  uint8_t step_{0};
  uint32_t last_trigger_ms_{0};
  uint32_t started_ms_{0};
  uint32_t due_ms_{0};
};

}  // namespace esphome::kvm_usb_bridge
