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
  void report_delivered(const uint8_t *report, uint16_t length);
  void report_failed() { output_failed_.store(true); report_in_flight_.store(false); }
  uint8_t leds() const { return leds_.load(); }
  void set_leds(uint8_t value) { leds_.store(value & 0x1F); led_revision_.fetch_add(1); }
  void set_idle(uint8_t value) { idle_rate_.store(value); }
  void set_host_enabled(bool value) { host_enabled_ = value; }
  void set_host_hub_support(bool value) { host_hub_support_ = value; }
  void set_output_phy(uint8_t value) { output_phy_ = value; }
  void set_swap_data_pins(bool value) { swap_data_pins_ = value; }
  void set_dump_descriptors(bool value) { dump_descriptors_ = value; }
  void set_switch_hotkey(bool value) { switch_hotkey_.set_enabled(value); }
  void set_trace_key_counts(bool value) { trace_key_counts_ = value; }
  void set_drain_auxiliary_interfaces(bool value) { drain_auxiliary_interfaces_ = value; }
  void set_media_controls(bool value) { media_controls_ = value; }
  ControlState current_controls();
  void controls_delivered() { media_delivered_.fetch_add(1); media_in_flight_.store(false); }
  void controls_failed() { media_failed_.store(true); media_in_flight_.store(false); }
  void set_media_idle(uint8_t value) { media_idle_rate_.store(value); }
  BootReport current_report();

 protected:
  struct Slot {
    KVMUSBBridge *owner{nullptr};
    std::atomic<bool> keyboard{false};
    std::atomic<bool> media{false};
    hid_host_device_handle_t handle{nullptr};
    KeyboardDescriptor descriptor;
    bool led_pending{false};
    uint8_t interface_number{0};
    uint16_t trace_counts{0xFFFF};
    uint16_t trace_length{0xFFFF};
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
  // Counts/times only: no usages, characters or raw input reports in traces.
  struct CountTrace {
    uint32_t at_ms{0}, sequence{0}, age_ms{0};
    uint16_t counts{0};
    uint8_t stage{0}, source{0}, queued{0};
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
  void recompute_controls_(bool fresh_control_report = false);
  void recover_controls_();
  void service_controls_();
  bool send_report_(const BootReport &report);
  void abort_switch_(const char *reason);
  void recover_output_(const char *reason, bool clear_inputs = false);
  void output_event_(uint8_t event);
  void reconnect_output_();
  void queue_control_(const ControlRequest &request);
  void service_leds_();
  void trace_counts_(uint8_t stage, uint8_t source, uint16_t counts, uint32_t at,
                     uint32_t sequence, uint32_t age, uint8_t queued);
  std::array<Slot, 8> slots_;
  ReportBuffer reports_;
  TimedReportBuffer<ControlPacket> media_reports_;
  ControlReleaseGate media_gate_;
  ControlState desired_controls_{}, last_controls_{};
  uint32_t media_last_send_ms_{0};
  std::atomic<bool> media_in_flight_{false}, media_failed_{false}, media_waiting_{false};
  std::atomic<uint32_t> media_queued_{0};
  std::atomic<uint8_t> media_idle_rate_{0}, media_count_{0};
  std::atomic<uint32_t> media_received_{0}, media_delivered_{0}, media_dropped_{0};
  ReleaseGate release_gate_;
  QueueHandle_t events_{nullptr};
  QueueHandle_t controls_{nullptr};
  QueueHandle_t count_traces_{nullptr};
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
  std::atomic<uint32_t> submitted_at_ms_{0};
  std::atomic<uint32_t> max_input_age_ms_{0}, max_output_wait_ms_{0}, max_ack_ms_{0}, max_output_depth_{0};
  std::atomic<uint32_t> trace_drops_{0};
  std::atomic<uint32_t> input_callbacks_{0}, empty_input_{0}, array_input_{0}, bitmap_input_{0}, other_input_{0};
  std::atomic<uint32_t> auxiliary_reports_{0}, auxiliary_errors_{0};
  std::atomic<uint8_t> auxiliary_count_{0};
  std::atomic<uint16_t> input_counts_{0}, desired_counts_{0}, acked_counts_{0};
  std::atomic<uint16_t> traced_ack_counts_{0xFFFF};
  std::atomic<bool> waiting_for_release_{false};
  uint16_t traced_desired_counts_{0xFFFF};
  uint32_t last_status_ms_{0};
  bool host_hub_support_{false};
  bool host_enabled_{true};
  uint8_t output_phy_{0};
  bool swap_data_pins_{false};
  bool dump_descriptors_{false};
  bool trace_key_counts_{false};
  bool drain_auxiliary_interfaces_{false};
  bool media_controls_{false};
  SwitchHotkey switch_hotkey_;
};

extern KVMUSBBridge *bridge_instance;

}  // namespace esphome::kvm_usb_bridge
