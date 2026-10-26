#pragma once

// Transport-independent HID keyboard descriptor parser. No key text is logged.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include "hid_controls.h"

namespace esphome::kvm_usb_bridge {

struct KeyState {
  std::array<uint8_t, 32> bits{};
  void press(uint32_t usage) {
    if (usage > 3 && usage < 256) bits[usage / 8] |= uint8_t(1u << (usage % 8));
  }
  bool down(uint32_t usage) const { return usage < 256 && (bits[usage / 8] & (1u << (usage % 8))); }
  void merge(const KeyState &other) {
    for (size_t i = 0; i < bits.size(); i++) bits[i] |= other.bits[i];
  }
};

struct BootReport {
  uint8_t modifiers{0};
  uint8_t reserved{0};
  uint8_t keys[6]{};
  bool operator==(const BootReport &other) const { return std::memcmp(this, &other, sizeof(*this)) == 0; }
  bool operator!=(const BootReport &other) const { return !(*this == other); }
};
static_assert(sizeof(BootReport) == 8, "Boot keyboard reports must be exactly eight bytes");

inline BootReport boot_report(const KeyState &state, uint8_t maximum = 0x65) {
  BootReport result;
  for (unsigned i = 0; i < 8; i++) if (state.down(0xE0 + i)) result.modifiers |= uint8_t(1u << i);
  unsigned count = 0;
  for (unsigned usage = 4; usage <= maximum && usage < 0xE0; usage++) {
    if (!state.down(usage)) continue;
    if (count == 6) {
      // HID ErrorRollOver, rather than silently dropping an arbitrary seventh key.
      std::memset(result.keys, 1, sizeof(result.keys));
      return result;
    }
    result.keys[count++] = uint8_t(usage);
  }
  return result;
}

class KeyboardDescriptor {
 public:
  struct Field {
    uint16_t bit_offset{0};
    uint16_t count{0};
    uint8_t size{0};
    bool variable{false};
    int32_t logical_min{0};
    uint32_t usage_min{0};
    uint32_t usage_max{0};
    std::vector<uint32_t> usages;
    uint32_t usage_at(uint32_t index) const {
      if (!usages.empty()) return index < usages.size() ? usages[index] : usages.back();
      if (usage_min + index <= usage_max) return usage_min + index;
      return usage_max;
    }
  };
  struct Layout {
    uint8_t id{0};
    uint16_t input_bits{0};
    uint16_t output_bits{0};
    std::vector<Field> keyboard;
    std::vector<Field> leds;
    std::vector<Field> consumer;
    std::vector<Field> system;
    KeyState state;
    ControlState controls;
  };

  bool parse(const uint8_t *data, size_t length, bool controls = false) {
    layouts_.clear();
    report_ids_ = false;
    struct Globals {
      uint32_t page{0}, size{0}, count{0};
      int32_t logical_min{0}, logical_max{0};
      uint8_t id{0};
    } g;
    std::array<Globals, 8> stack{};
    size_t depth = 0;
    std::vector<uint32_t> usages;
    uint32_t usage_min = 0, usage_max = 0;
    bool has_range = false;
    unsigned total_fields = 0;
    for (size_t pos = 0; pos < length;) {
      uint8_t prefix = data[pos++];
      if (prefix == 0xFE) {
        if (pos + 2 > length) return false;
        size_t long_size = data[pos];
        pos += 2;
        if (long_size > length - pos) return false;
        pos += long_size;
        continue;
      }
      size_t size = prefix & 3;
      if (size == 3) size = 4;
      if (size > length - pos) return false;
      uint32_t value = 0;
      for (size_t i = 0; i < size; i++) value |= uint32_t(data[pos++]) << (i * 8);
      unsigned type = (prefix >> 2) & 3, tag = prefix >> 4;
      if (type == 1) {
        switch (tag) {
          case 0: g.page = value; break;
          case 1:
            g.logical_min = size == 1 ? int8_t(value) : size == 2 ? int16_t(value) : int32_t(value);
            break;
          case 2: g.logical_max = int32_t(value); break;
          case 7: g.size = value; break;
          case 8:
            if (value == 0 || value > 255) return false;
            g.id = uint8_t(value);
            report_ids_ = true;
            break;
          case 9: g.count = value; break;
          case 10:
            if (depth == stack.size()) return false;
            stack[depth++] = g;
            break;
          case 11:
            if (depth == 0) return false;
            g = stack[--depth];
            break;
        }
      } else if (type == 2) {
        uint32_t usage = size == 4 ? value : ((g.page << 16) | value);
        if (tag == 0) {
          if (usages.size() >= 256) return false;
          usages.push_back(usage);
        } else if (tag == 1) {
          usage_min = usage;
          has_range = true;
        } else if (tag == 2) {
          usage_max = usage;
        }
      } else if (type == 0) {
        if (tag == 8 || tag == 9) {
          // Count all bits, including padding, mouse and vendor-defined fields.
          if (g.size > 32 || g.count > 8192 || g.size * g.count > 8192) return false;
          auto *layout = get_layout_(g.id);
          if (layout == nullptr) return false;
          uint16_t &offset = tag == 8 ? layout->input_bits : layout->output_bits;
          if (offset + g.size * g.count > 8192) return false;
          const uint32_t first_usage = usages.empty() ? (has_range ? usage_min : (g.page << 16)) : usages.front();
          const unsigned page = first_usage >> 16;
          const bool button = !(value & 2) || (g.logical_min == 0 && g.logical_max == 1);
          const bool consumer = controls && tag == 8 && page == 12 && !(value & 4) && button;
          bool system = false;
          if (controls && tag == 8 && page == 1 && !(value & 4) && button) {
            for (auto usage : usages) if (usage >= 0x00010081 && usage <= 0x00010083) system = true;
            if (has_range && usage_min <= 0x00010083 && usage_max >= 0x00010081) system = true;
          }
          if (!(value & 1) && g.size != 0 && g.count != 0 &&
              ((tag == 8 && page == 7) || (tag == 9 && page == 8) || consumer || system)) {
            if (++total_fields > 64 || g.count > 256) return false;
            Field f;
            f.bit_offset = offset;
            f.size = uint8_t(g.size);
            f.count = uint16_t(g.count);
            f.variable = value & 2;
            f.logical_min = g.logical_min;
            f.usages = usages;
            f.usage_min = has_range ? usage_min : first_usage;
            f.usage_max = has_range ? usage_max : first_usage;
            if (f.usage_max < f.usage_min) return false;
            (consumer ? layout->consumer : system ? layout->system : tag == 8 ? layout->keyboard : layout->leds).push_back(std::move(f));
          }
          offset += uint16_t(g.size * g.count);
        }
        usages.clear();
        usage_min = usage_max = 0;
        has_range = false;
      }
    }
    return has_keyboard() || has_controls();
  }

  bool update(const uint8_t *data, size_t length) {
    if (length == 0) return false;
    const uint8_t id = report_ids_ ? *data++ : 0;
    if (report_ids_) length--;
    for (auto &layout : layouts_) {
      if (layout.id != id || (layout.keyboard.empty() && layout.consumer.empty() && layout.system.empty())) continue;
      if (length < (layout.input_bits + 7u) / 8u) return false;
      KeyState next;
      for (const auto &f : layout.keyboard) {
        for (unsigned i = 0; i < f.count; i++) {
          uint32_t value = read_bits_(data, f.bit_offset + i * f.size, f.size);
          uint32_t usage;
          if (f.variable) {
            if (value == 0) continue;
            usage = f.usage_at(i);
          } else {
            const int64_t index = int64_t(value) - f.logical_min;
            if (index < 0) continue;
            if (!f.usages.empty() && uint64_t(index) >= f.usages.size()) continue;
            if (f.usages.empty() && uint64_t(index) > f.usage_max - f.usage_min) continue;
            usage = f.usage_at(uint32_t(index));
            // An input rollover/error report must not release previously held keys.
            if ((usage >> 16) == 7 && (usage & 0xFFFF) >= 1 && (usage & 0xFFFF) <= 3) return false;
          }
          if ((usage >> 16) == 7) next.press(usage & 0xFFFF);
        }
      }
      ControlState next_controls;
      for (const auto *fields : {&layout.consumer, &layout.system}) {
        for (const auto &f : *fields) for (unsigned i = 0; i < f.count; i++) {
          const uint32_t value = read_bits_(data, f.bit_offset + i * f.size, f.size);
          uint32_t usage = 0;
          if (f.variable) {
            if (value > 1) return false;
            if (value == 0) continue;
            usage = f.usage_at(i);
          } else {
            const int64_t index = int64_t(value) - f.logical_min;
            if (index < 0 || (!f.usages.empty() && uint64_t(index) >= f.usages.size()) ||
                (f.usages.empty() && uint64_t(index) > f.usage_max - f.usage_min)) continue;
            usage = f.usage_at(uint32_t(index));
          }
          if ((usage >> 16) == 12 && !next_controls.add(uint16_t(usage))) return false;
          if (usage >= 0x00010081 && usage <= 0x00010083)
            next_controls.system |= 1u << (usage - 0x00010081);
        }
      }
      ControlState merged = next_controls;
      for (const auto &other : layouts_) if (&other != &layout && !merged.merge(other.controls)) return false;
      layout.state = next;
      layout.controls = next_controls;
      return true;
    }
    return false;
  }

  KeyState state() const {
    KeyState merged;
    for (const auto &layout : layouts_) merged.merge(layout.state);
    return merged;
  }
  bool has_keyboard() const {
    for (const auto &layout : layouts_) if (!layout.keyboard.empty()) return true;
    return false;
  }
  bool has_controls() const {
    for (const auto &layout : layouts_) if (!layout.consumer.empty() || !layout.system.empty()) return true;
    return false;
  }
  bool keyboard_report(const uint8_t *data, size_t length) const {
    const auto *layout = input_layout_(data, length);
    return layout != nullptr && !layout->keyboard.empty();
  }
  bool controls_report(const uint8_t *data, size_t length) const {
    const auto *layout = input_layout_(data, length);
    return layout != nullptr && (!layout->consumer.empty() || !layout->system.empty());
  }
  ControlState controls() const {
    ControlState result;
    for (const auto &layout : layouts_) if (!result.merge(layout.controls)) return ControlState{};
    return result;
  }
  void clear_controls() { for (auto &layout : layouts_) layout.controls = ControlState{}; }
  const std::vector<Layout> &layouts() const { return layouts_; }
  void clear_state() { for (auto &layout : layouts_) { layout.state = KeyState{}; layout.controls = ControlState{}; } }
  bool has_report_ids() const { return report_ids_; }

  // Produce the complete SET_REPORT payload, including an ID byte when present.
  std::vector<uint8_t> led_report(const Layout &layout, uint8_t leds) const {
    if (layout.leds.empty()) return {};
    const size_t prefix = report_ids_ ? 1 : 0;
    std::vector<uint8_t> result(prefix + (layout.output_bits + 7) / 8, 0);
    if (prefix) result[0] = layout.id;
    for (const auto &f : layout.leds) {
      if (!f.variable) continue;
      for (unsigned i = 0; i < f.count; i++) {
        const unsigned usage = f.usage_at(i) & 0xFFFF;
        if (usage >= 1 && usage <= 5 && (leds & (1u << (usage - 1)))) {
          unsigned bit = unsigned(prefix * 8) + f.bit_offset + i * f.size;
          result[bit / 8] |= uint8_t(1u << (bit % 8));
        }
      }
    }
    return result;
  }

 private:
  const Layout *input_layout_(const uint8_t *data, size_t length) const {
    if (length == 0) return nullptr;
    const uint8_t id = report_ids_ ? data[0] : 0;
    for (const auto &layout : layouts_) if (layout.id == id) return &layout;
    return nullptr;
  }
  Layout *get_layout_(uint8_t id) {
    for (auto &layout : layouts_) if (layout.id == id) return &layout;
    if (layouts_.size() == 16) return nullptr;
    layouts_.push_back(Layout{});
    layouts_.back().id = id;
    return &layouts_.back();
  }
  static uint32_t read_bits_(const uint8_t *data, unsigned offset, unsigned size) {
    uint32_t result = 0;
    for (unsigned i = 0; i < size; i++) if (data[(offset + i) / 8] & (1u << ((offset + i) % 8))) result |= uint32_t(1) << i;
    return result;
  }
  bool report_ids_{false};
  std::vector<Layout> layouts_;
};

}  // namespace esphome::kvm_usb_bridge
