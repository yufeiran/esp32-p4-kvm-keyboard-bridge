#pragma once

#include "esphome/core/component.h"
#include "hid_keyboard.h"
#include "switch_hotkey.h"
#include "report_buffer.h"
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
  uint32_t recovery_count() const { return recoveries_.load(); }
  uint32_t stale_reports() const { return stale_.load(); }
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
    uint32_t at_ms{0};
    uint8_t data[256]{};
  };
  enum class ControlType : uint8_t { LED, CLOSE, RESTART };
  struct ControlRequest {
    hid_host_device_handle_t handle{nullptr};
    ControlType type{ControlType::LED};
    uint8_t report_id{0};
    uint16_t length{0};
    uint8_t data[64]{};
  };
  static void host_daemon_(void *arg);
  static void bridge_task_(void *arg);
  static void control_task_(void *arg);
  static void connected_(hid_host_device_handle_t handle, hid_host_driver_event_t event, void *arg);
  static void interface_event_(hid_host_device_handle_t handle, hid_host_interface_event_t event, void *arg);
  void open_keyboard_(hid_host_device_handle_t handle);
  void handle_input_(const Event &event);
  void recompute_(bool fresh_physical_report = false);
  void service_output_();
  bool send_report_(const BootReport &report);
  void abort_switch_(const char *reason);
  void recover_output_(const char *reason, bool clear_inputs = false);
  void output_event_(uint8_t event);
  void reconnect_output_();
  void queue_control_(const ControlRequest &request);
  void service_leds_();
  std::array<Slot, 8> slots_;
  ReportBuffer reports_;
  ReleaseGate release_gate_;
  QueueHandle_t events_{nullptr};
  QueueHandle_t controls_{nullptr};
  BootReport desired_{};
  BootReport last_enqueued_{};
  portMUX_TYPE state_mux_ = portMUX_INITIALIZER_UNLOCKED;
  bool was_mounted_{false};
  bool input_boundary_valid_{false};
  uint32_t input_boundary_ms_{0};
  uint32_t seen_output_revision_{0};
  uint32_t last_worker_ms_{0};
  uint32_t detach_started_ms_{0};
  uint32_t last_reconnect_ms_{0};
  bool detached_for_recovery_{false};
  bool have_reconnected_{false};
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
  std::atomic<uint32_t> output_revision_{0};
  std::atomic<bool> awaiting_output_mount_{false};
  std::atomic<uint32_t> recoveries_{0};
  std::atomic<uint32_t> stale_{0};
  std::atomic<uint32_t> input_errors_{0};
  std::atomic<uint32_t> control_errors_{0};
  std::atomic<uint32_t> max_control_ms_{0};
  std::atomic<uint32_t> max_worker_gap_ms_{0};
  std::atomic<uint32_t> queued_output_{0};
  std::atomic<uint32_t> in_flight_age_ms_{0};
  uint32_t last_status_ms_{0};
  bool host_enabled_{true};
  uint8_t output_phy_{0};
  bool swap_data_pins_{false};
  bool dump_descriptors_{false};
  SwitchHotkey switch_hotkey_;
};

extern KVMUSBBridge *bridge_instance;

}  // namespace esphome::kvm_usb_bridge
