#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace esphome::kvm_usb_bridge {

// Standard Consumer usages and the three Generic Desktop System controls.
// No vendor protocol, mouse data or characters are represented here.
struct ControlState {
  static constexpr unsigned CAPACITY = 8;
  std::array<uint16_t, CAPACITY> consumer{};
  uint8_t system{0};
  bool add(uint16_t usage) {
    if (usage == 0 || usage > 0x03FF) return true;
    for (auto value : consumer) if (value == usage) return true;
    for (auto &value : consumer) if (value == 0) {
      value = usage;
      std::sort(consumer.begin(), consumer.end(), [](uint16_t a, uint16_t b) {
        return a != 0 && (b == 0 || a < b);
      });
      return true;
    }
    return false;
  }
  bool merge(const ControlState &other) {
    system |= other.system;
    for (auto value : other.consumer) if (!add(value)) return false;
    return true;
  }
  bool empty() const { return system == 0 && consumer[0] == 0; }
  bool operator==(const ControlState &other) const { return system == other.system && consumer == other.consumer; }
  bool operator!=(const ControlState &other) const { return !(*this == other); }
};

struct ControlPacket {
  uint8_t id{1};
  uint8_t length{16};
  std::array<uint8_t, 16> data{};
  static ControlPacket consumer(const ControlState &state) {
    ControlPacket result;
    for (unsigned i = 0; i < state.consumer.size(); i++) {
      result.data[2 * i] = uint8_t(state.consumer[i]);
      result.data[2 * i + 1] = uint8_t(state.consumer[i] >> 8);
    }
    return result;
  }
  static ControlPacket system(uint8_t value) {
    ControlPacket result;
    result.id = 2;
    result.length = 1;
    result.data[0] = value & 7;
    return result;
  }
};

class ControlReleaseGate {
 public:
  void arm() { waiting_ = true; }
  // An idle recovery has no held control to quarantine. Requiring a new zero
  // there consumes the first real encoder pulse; repeated recovery must still
  // retain an existing quarantine until a physical release arrives.
  void arm_if_held(const ControlState &state) { waiting_ = waiting_ || !state.empty(); }
  bool waiting() const { return waiting_; }
  void observe(const ControlState &state, bool fresh_control_report) {
    if (fresh_control_report && state.empty()) waiting_ = false;
  }
 private:
  bool waiting_{false};
};

}  // namespace esphome::kvm_usb_bridge
