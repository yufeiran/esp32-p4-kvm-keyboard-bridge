#pragma once

#include "esphome/core/component.h"
#include "hid_keyboard.h"
#include "switch_hotkey.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
// Espressif's host driver uses a packed enum, TinyUSB uses an unpacked enum,
// and both call it hid_report_type_t. Keep their C APIs and ABIs separate.
#define hid_report_type_t host_hid_report_type_t
#define HID_REPORT_TYPE_INPUT HOST_HID_REPORT_TYPE_INPUT
#define HID_REPORT_TYPE_OUTPUT HOST_HID_REPORT_TYPE_OUTPUT
#define HID_REPORT_TYPE_FEATURE HOST_HID_REPORT_TYPE_FEATURE
#include "usb/hid_host.h"
#undef hid_report_type_t
#undef HID_REPORT_TYPE_INPUT
#undef HID_REPORT_TYPE_OUTPUT
#undef HID_REPORT_TYPE_FEATURE
#include <atomic>

namespace esphome::kvm_usb_bridge {

class KVMUSBBridge : public Component {
 public:
  void setup() override;
  void dump_config() override;
  void loop() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }
  bool input_connected() const { return input_count_.load() != 0; }
  bool output_connected() const { return mounted_.load(); }
  uint32_t rejected_reports() const { return rejected_.load(); }
  uint32_t queue_overflows() const { return overflows_.load(); }
  uint32_t received_reports() const { return received_.load(); }
  uint32_t delivered_reports() const { return delivered_.load(); }
  void report_delivered() { delivered_.fetch_add(1); report_in_flight_.store(false); }
  void report_failed() { output_failed_.store(true); report_in_flight_.store(false); }
  uint8_t leds() const { return leds_.load(); }
  void set_leds(uint8_t value) { leds_.store(value & 0x1F); led_revision_.fetch_add(1); }
  void set_idle(uint8_t value) { idle_rate_.store(value); }
  void set_host_enabled(bool value) { host_enabled_ = value; }
  void set_output_phy(uint8_t value) { output_phy_ = value; }
  void set_swap_data_pins(bool value) { swap_data_pins_ = value; }
  void set_dump_descriptors(bool value) { dump_descriptors_ = value; }
  void set_switch_hotkey(bool value) { switch_hotkey_.set_enabled(value); }
  BootReport current_report();

 protected:
  struct Slot {
    hid_host_device_handle_t handle{nullptr};
    KeyboardDescriptor descriptor;
    bool led_pending{false};
  };
  struct Event {
    hid_host_device_handle_t handle{nullptr};
    bool connected{false};
    hid_host_interface_event_t type{};
    size_t length{0};
    uint8_t data[256]{};
  };
  static void host_daemon_(void *arg);
  static void bridge_task_(void *arg);
  static void connected_(hid_host_device_handle_t handle, hid_host_driver_event_t event, void *arg);
  static void interface_event_(hid_host_device_handle_t handle, hid_host_interface_event_t event, void *arg);
  void open_keyboard_(hid_host_device_handle_t handle);
  void handle_input_(const Event &event);
  void recompute_();
  void service_output_();
  bool send_report_(const BootReport &report);
  void abort_switch_(const char *reason);
  void service_leds_();
  std::array<Slot, 8> slots_;
  QueueHandle_t reports_{nullptr};
  QueueHandle_t events_{nullptr};
  BootReport desired_{};
  BootReport pending_{};
  BootReport last_enqueued_{};
  portMUX_TYPE state_mux_ = portMUX_INITIALIZER_UNLOCKED;
  bool pending_valid_{false};
  bool was_mounted_{false};
  uint32_t seen_led_revision_{0};
  std::atomic<uint8_t> idle_rate_{0};
  uint32_t last_send_ms_{0};
  std::atomic<bool> mounted_{false};
  std::atomic<uint8_t> input_count_{0};
  std::atomic<uint8_t> leds_{0};
  std::atomic<uint32_t> led_revision_{0};
  std::atomic<uint32_t> rejected_{0};
  std::atomic<uint32_t> overflows_{0};
  std::atomic<uint32_t> received_{0};
  std::atomic<uint32_t> delivered_{0};
  std::atomic<bool> report_in_flight_{false};
  std::atomic<bool> output_failed_{false};
  std::atomic<bool> running_{false};
  std::atomic<bool> worker_failed_{false};
  std::atomic<bool> input_overflow_{false};
  uint32_t last_status_ms_{0};
  bool host_enabled_{true};
  uint8_t output_phy_{0};
  bool swap_data_pins_{false};
  bool dump_descriptors_{false};
  SwitchHotkey switch_hotkey_;
};

extern KVMUSBBridge *bridge_instance;

}  // namespace esphome::kvm_usb_bridge
