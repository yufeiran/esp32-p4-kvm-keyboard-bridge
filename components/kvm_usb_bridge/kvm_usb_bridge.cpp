#include "kvm_usb_bridge.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esp_intr_alloc.h"
#include "driver/gpio.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_private/periph_ctrl.h"
#include "hal/usb_serial_jtag_ll.h"
#include "hal/usb_wrap_ll.h"
#include "hal/usb_dwc_ll.h"
#include "soc/lp_system_struct.h"
#include "soc/usb_wrap_struct.h"
#include "soc/io_mux_reg.h"
#include "soc/usb_serial_jtag_reg.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"
#include "class/hid/hid_device.h"

namespace esphome::kvm_usb_bridge {

static const char *const TAG = "kvm_usb_bridge";
KVMUSBBridge *bridge_instance = nullptr;

// Low byte = ordinary keys held, high byte = modifiers held. Never identify a key.
static uint16_t key_counts(const KeyState &state) {
  uint16_t result = 0;
  for (unsigned usage = 4; usage < 256; usage++)
    if (state.down(usage)) result += usage >= 0xE0 && usage <= 0xE7 ? 0x100 : 1;
  return result;
}
static uint16_t key_counts(const BootReport &report) {
  uint16_t result = 0;
  for (auto key : report.keys) if (key > 3) result++;
  for (unsigned i = 0; i < 8; i++) if (report.modifiers & (1u << i)) result += 0x100;
  return result;
}

// Deliberately one keyboard interface, no Report ID and no vendor/media/mouse collections.
static const uint8_t REPORT_DESCRIPTOR[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07,
    0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01,
    0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07,
    0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0,
};
static const uint8_t CONFIG_DESCRIPTOR[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN, 0, 500),
    TUD_HID_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_KEYBOARD, sizeof(REPORT_DESCRIPTOR), 0x81, 8, 1),
};
static const tusb_desc_device_t DEVICE_DESCRIPTOR = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0110,
    .bDeviceClass = 0,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = 64,
    .idVendor = 0x303A,
    .idProduct = 0x4004,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 0,
    .bNumConfigurations = 1,
};
static const char *STRINGS[] = {"\x09\x04", "P4 Keyboard Bridge", "KVM Boot Keyboard"};

void KVMUSBBridge::setup() {
  bridge_instance = this;
  events_ = xQueueCreate(64, sizeof(Event));
  controls_ = xQueueCreate(32, sizeof(ControlRequest));
  if (events_ == nullptr || controls_ == nullptr) { mark_failed(); return; }
  if (trace_key_counts_) count_traces_ = xQueueCreate(64, sizeof(CountTrace));

  // PHY0 (GPIO24/25) has its own 48MHz clock under the USJ clock gate.
  // FS OTG setup enables PHY1's clock only. Enable PHY0 before swapping.
  PERIPH_RCC_ATOMIC() { usb_serial_jtag_ll_enable_bus_clock(true); }
  // Leave the default route until TinyUSB enables the FS controller and PHY1
  // clock. The swap must take effect with BOTH physical PHY clocks running.
  usb_wrap_ll_phy_select(&USB_WRAP, 1);
  usb_serial_jtag_pull_override_vals_t native_pull{};
  usb_serial_jtag_ll_phy_enable_pull_override(&native_pull);
  // ROM exposes Serial/JTAG on GPIO24/25 during reset. Disconnect that PHY
  // before swapping it, so the KVM releases the previous USB address/device.
  // DWC's soft disconnect alone does not remove the ROM controller's pull-up.
  usb_serial_jtag_ll_phy_enable_pad(false);
  vTaskDelay(pdMS_TO_TICKS(1000));

  // IDF's usb_new_phy configures drive strength on the default GPIO26/27 only.
  // The swapped PHY on GPIO24/25 needs the same mandatory 40mA setting.
  const gpio_num_t dm = output_phy_ == 0 ? GPIO_NUM_24 : GPIO_NUM_26;
  const gpio_num_t dp = output_phy_ == 0 ? GPIO_NUM_25 : GPIO_NUM_27;
  gpio_drive_cap_t old_dm{}, old_dp{};
  gpio_get_drive_capability(dm, &old_dm);
  gpio_get_drive_capability(dp, &old_dp);
  // Normal boot leaves SLP_SEL set on these pads; ROM USB download clears it.
  // Select the active pad settings explicitly when taking ownership of USB.
  if (gpio_sleep_sel_dis(dm) != ESP_OK || gpio_sleep_sel_dis(dp) != ESP_OK ||
      gpio_set_drive_capability(dm, GPIO_DRIVE_CAP_3) != ESP_OK ||
      gpio_set_drive_capability(dp, GPIO_DRIVE_CAP_3) != ESP_OK) {
    ESP_LOGE(TAG, "Cannot configure USB output pads");
    mark_failed();
    return;
  }
  ESP_LOGD(TAG, "USB GPIO%u/%u drive strength: %u/%u -> 3/3", unsigned(dm), unsigned(dp),
           unsigned(old_dm), unsigned(old_dp));

  tinyusb_config_t device = TINYUSB_DEFAULT_CONFIG();
  device.port = TINYUSB_PORT_FULL_SPEED_0;
  device.descriptor.device = &DEVICE_DESCRIPTOR;
  device.descriptor.string = STRINGS;
  device.descriptor.string_count = 3;
  device.descriptor.full_speed_config = CONFIG_DESCRIPTOR;
  device.event_cb = [](tinyusb_event_t *event, void *arg) {
    static_cast<KVMUSBBridge *>(arg)->output_event_(uint8_t(event->id));
  };
  device.event_arg = this;
  esp_err_t error = tinyusb_driver_install(&device);
  if (error != ESP_OK) {
    ESP_LOGE(TAG, "FS keyboard initialization failed: %s", esp_err_to_name(error));
    mark_failed();
    return;
  }
  // Waveshare H2 is wired to GPIO24(D-)/25(D+), not the default GPIO26/27.
  // Set this AFTER TinyUSB creates its PHY; CH343/UART0 is unaffected.
  tud_disconnect();
  usb_wrap_ll_phy_select(&USB_WRAP, output_phy_);
  usb_wrap_ll_phy_enable_pin_exchg(&USB_WRAP, swap_data_pins_);
  // The ROM's Serial/JTAG device can already be attached on H2. Force an
  // actual disconnect on the newly routed PHY before exposing new descriptors.
  vTaskDelay(pdMS_TO_TICKS(100));
  tud_connect();
  usb_serial_jtag_ll_phy_enable_pad(true);
  native_pull.dp_pu = true;
  usb_serial_jtag_ll_phy_enable_pull_override(&native_pull);
  usb_serial_jtag_ll_phy_disable_pull_override();

  ESP_LOGI(TAG, "Output PHY: %s, D+/D- swap: %s, input host: %s", output_phy_ == 0 ? "GPIO24/25" : "GPIO26/27",
           swap_data_pins_ ? "yes" : "no", host_enabled_ ? "enabled" : "disabled (device-only test)");
  ESP_LOGD(TAG, "USB fuse settings (read only): PHY_SEL=%u, DEVICE_EXCHG=%u, OTG11_EXCHG=%u",
           esp_efuse_read_field_bit(ESP_EFUSE_USB_PHY_SEL), esp_efuse_read_field_bit(ESP_EFUSE_USB_DEVICE_EXCHG_PINS),
           esp_efuse_read_field_bit(ESP_EFUSE_USB_OTG11_EXCHG_PINS));
  running_.store(true);
  if (host_enabled_) {
  usb_host_config_t host{};
  host.intr_flags = ESP_INTR_FLAG_LEVEL1;
  // IDF 5.5 Host numbers HS=0, FS=1; TinyUSB numbers FS=0, HS=1.
  host.peripheral_map = 1u << 0;  // HS-capable input controller; physical route selected by board jumper.
  error = usb_host_install(&host);
  if (error != ESP_OK) {
    ESP_LOGE(TAG, "HS host initialization failed: %s", esp_err_to_name(error));
    mark_failed();
    return;
  }
  ESP_LOGI(TAG, "Input host: %s", host_hub_support_ ? "single hub or direct, FS/LS only" : "direct only");
  // Disconnect/hub cleanup must outrank HID callbacks (priority 5),
  // especially when changing the board's physical USB route.
  if (xTaskCreate(host_daemon_, "kvm_usb_host", 4096, this, 6, nullptr) != pdPASS) {
    running_.store(false); mark_failed(); return;
  }
  hid_host_driver_config_t hid{};
  hid.create_background_task = true;
  hid.task_priority = 5;
  hid.stack_size = 6144;
  hid.core_id = tskNO_AFFINITY;
  hid.callback = connected_;
  hid.callback_arg = this;
  error = hid_host_install(&hid);
  if (error != ESP_OK) {
    ESP_LOGE(TAG, "HID driver initialization failed: %s", esp_err_to_name(error));
    mark_failed();
    return;
  }
  }
  if (xTaskCreate(control_task_, "kvm_usb_ctrl", 4096, this, 4, nullptr) != pdPASS ||
      xTaskCreate(bridge_task_, "kvm_usb_bridge", 8192, this, 5, nullptr) != pdPASS) {
    mark_failed(); return;
  }
  ESP_LOGI(TAG, "Ready: USB-A host -> FS boot keyboard on native USB-C (GPIO24/25)");
}

void KVMUSBBridge::host_daemon_(void *arg) {
  auto *self = static_cast<KVMUSBBridge *>(arg);
  while (self->running_.load()) {
    uint32_t flags = 0;
    const esp_err_t error = usb_host_lib_handle_events(pdMS_TO_TICKS(100), &flags);
    if (error != ESP_OK && error != ESP_ERR_TIMEOUT) {
      self->worker_failed_.store(true);
      break;
    }
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
  }
  vTaskDelete(nullptr);
}

void KVMUSBBridge::bridge_task_(void *arg) {
  auto *self = static_cast<KVMUSBBridge *>(arg);
  while (self->running_.load()) {
    const uint32_t now = millis();
    const uint32_t gap = self->last_worker_ms_ == 0 ? 0 : now - self->last_worker_ms_;
    self->last_worker_ms_ = now;
    if (gap > self->max_worker_gap_ms_.load()) self->max_worker_gap_ms_.store(gap);
    if (gap > MAX_REPORT_AGE_MS) self->recover_output_("USB worker delayed; old transitions discarded", true);
    self->service_output_();
    // Blocking HID control requests must run outside the driver's event task.
    // Copy each input report in its callback; fetching "latest" here loses edges.
    Event event;
    const bool overflow = self->input_overflow_.exchange(false);
    if (overflow) self->recover_output_("Input queue overflow; release-only recovery", true);
    for (unsigned count = 0; count < 64 && xQueueReceive(self->events_, &event, 0) == pdPASS; count++) {
      if (event.connected) self->open_keyboard_(event.handle);
      else if (!overflow || event.type != HID_HOST_INTERFACE_EVENT_INPUT_REPORT) self->handle_input_(event);
    }
    self->service_output_();
    self->service_leds_();
    self->queued_output_.store(self->reports_.size());
    self->in_flight_age_ms_.store(self->report_in_flight_.load() ? millis() - self->last_send_ms_ : 0);
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  // Best effort release if a worker fails; leave the ESPHome runtime alive for diagnostics.
  if (tud_hid_ready()) {
    BootReport empty;
    tud_hid_report(0, &empty, sizeof(empty));
  }
  vTaskDelete(nullptr);
}

void KVMUSBBridge::control_task_(void *arg) {
  auto *self = static_cast<KVMUSBBridge *>(arg);
  while (self->running_.load()) {
    ControlRequest request;
    if (xQueueReceive(self->controls_, &request, pdMS_TO_TICKS(100)) != pdPASS) continue;
    // A failed interrupt transfer is not automatically rearmed by the driver.
    // Immediate stop/start can spin on a disappearing device, flooding UART
    // and starving the main loop. One serialized worker bounds aggregate
    // restart attempts to <=10/s. Only error recovery waits; key forwarding
    // has its own task and disconnect releases are processed immediately.
    if (request.type == ControlType::RESTART) vTaskDelay(pdMS_TO_TICKS(100));
    const uint32_t started = millis();
    esp_err_t error;
    if (request.type == ControlType::LED) {
      error = hid_class_request_set_report(request.handle, HOST_HID_REPORT_TYPE_OUTPUT,
                                           request.report_id, request.data, request.length);
    } else if (request.type == ControlType::CLOSE) {
      error = hid_host_device_close(request.handle);
    } else {
      error = hid_host_device_stop(request.handle);
      if (error == ESP_OK) error = hid_host_device_start(request.handle);
    }
    const uint32_t elapsed = millis() - started;
    if (elapsed > self->max_control_ms_.load()) self->max_control_ms_.store(elapsed);
    if (error != ESP_OK) {
      self->control_errors_.fetch_add(1);
      ESP_LOGW(TAG, "HID control operation %u failed after %ums: %s", unsigned(request.type), unsigned(elapsed), esp_err_to_name(error));
    } else if (elapsed > MAX_REPORT_AGE_MS) {
      ESP_LOGW(TAG, "HID control operation %u took %ums; key forwarding remained independent", unsigned(request.type), unsigned(elapsed));
    }
  }
  vTaskDelete(nullptr);
}

void KVMUSBBridge::queue_control_(const ControlRequest &request) {
  if (xQueueSend(controls_, &request, 0) == pdPASS) return;
  control_errors_.fetch_add(1);
  ESP_LOGW(TAG, "HID control queue full (operation %u)", unsigned(request.type));
  // LED requests are optional. Cleanup must remain serialized with any LED
  // transfer using the same handle; never close it concurrently as a fallback.
  // Half the queue is reserved for critical requests, so this wait is only
  // possible in a device/error storm. Input timestamps prevent delayed replay.
  if (request.type != ControlType::LED) xQueueSend(controls_, &request, portMAX_DELAY);
}

void KVMUSBBridge::connected_(hid_host_device_handle_t handle, hid_host_driver_event_t event, void *arg) {
  if (event != HID_HOST_DRIVER_EVENT_CONNECTED) return;
  auto *self = static_cast<KVMUSBBridge *>(arg);
  Event queued;
  queued.handle = handle;
  queued.connected = true;
  if (xQueueSend(self->events_, &queued, 0) != pdPASS) {
    self->input_overflow_.store(true);
    self->overflows_.fetch_add(1);
  }
}

void KVMUSBBridge::interface_event_(hid_host_device_handle_t handle, hid_host_interface_event_t event, void *arg) {
  auto *slot = static_cast<Slot *>(arg);
  auto *self = slot->owner;
  if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT && !slot->keyboard.load()) {
    // Reading the endpoint keeps the receiver's other interfaces serviced.
    // Do not copy, parse, log or forward their mouse/vendor/media payloads.
    self->auxiliary_reports_.fetch_add(1);
    return;
  }
  Event queued;
  queued.handle = handle;
  queued.type = event;
  queued.at_ms = millis();
  if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) self->input_callbacks_.fetch_add(1);
  if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT &&
      hid_host_device_get_raw_input_report_data(handle, queued.data, sizeof(queued.data), &queued.length) != ESP_OK) {
    self->rejected_.fetch_add(1);
    return;
  }
  if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
    if (queued.length == 0) self->empty_input_.fetch_add(1);
    else if (queued.length == 8) self->array_input_.fetch_add(1);
    else if (queued.length == 19) self->bitmap_input_.fetch_add(1);
    else self->other_input_.fetch_add(1);
  }
  // Reserve space for connection/disconnection events during an input burst.
  if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT && uxQueueSpacesAvailable(self->events_) <= 16) {
    self->input_overflow_.store(true);
    self->overflows_.fetch_add(1);
    return;
  }
  if (xQueueSend(self->events_, &queued, 0) != pdPASS) {
    self->input_overflow_.store(true);
    self->overflows_.fetch_add(1);
  }
}

void KVMUSBBridge::open_keyboard_(hid_host_device_handle_t handle) {
  hid_host_dev_params_t params{};
  if (hid_host_device_get_params(handle, &params) != ESP_OK) return;
  Slot *slot = nullptr;
  for (auto &entry : slots_) if (entry.handle == nullptr) { slot = &entry; break; }
  if (slot == nullptr) { ESP_LOGW(TAG, "No free HID interface slot"); return; }
  slot->owner = this;
  slot->keyboard.store(false);
  slot->handle = handle;
  hid_host_device_config_t config{};
  config.callback = interface_event_;
  config.callback_arg = slot;
  esp_err_t error = hid_host_device_open(handle, &config);
  if (error != ESP_OK) {
    slot->handle = nullptr;
    ESP_LOGW(TAG, "Cannot open HID interface %u: %s", params.iface_num, esp_err_to_name(error));
    return;
  }
  size_t length = 0;
  uint8_t *descriptor = hid_host_get_report_descriptor(handle, &length);
  const bool keyboard = descriptor != nullptr && length <= 2048 && slot->descriptor.parse(descriptor, length);
  if (!keyboard && !drain_auxiliary_interfaces_) {
    ESP_LOGD(TAG, "Skipping non-keyboard/unsupported HID interface %u", params.iface_num);
    hid_host_device_close(handle);
    slot->handle = nullptr;
    slot->descriptor = KeyboardDescriptor{};
    return;
  }
  if (!keyboard) slot->descriptor = KeyboardDescriptor{};
  if (dump_descriptors_ && descriptor != nullptr && length <= 2048) {
    // Static USB metadata only, never an input report or typed characters.
    ESP_LOGI(TAG, "HID-DESC-BEGIN interface=%u length=%u", params.iface_num, unsigned(length));
    for (size_t offset = 0; offset < length; offset += 24) {
      char hex[24 * 3 + 1]{};
      for (size_t i = 0; i < 24 && offset + i < length; i++)
        snprintf(hex + 3 * i, 4, "%02X ", descriptor[offset + i]);
      ESP_LOGD(TAG, "HID-DESC interface=%u offset=%u: %s", params.iface_num, unsigned(offset), hex);
    }
  }
  // Parse the original descriptor, so both real 6KRO and bitmap NKRO inputs work.
  if (keyboard && params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
    error = hid_class_request_set_protocol(handle, HID_REPORT_PROTOCOL_REPORT);
    if (error != ESP_OK) {
      ESP_LOGW(TAG, "Input protocol request failed on interface %u: %s", params.iface_num, esp_err_to_name(error));
      hid_host_device_close(handle);
      slot->handle = nullptr;
      slot->descriptor = KeyboardDescriptor{};
      return;
    }
  }
  slot->handle = handle;
  slot->keyboard.store(keyboard);
  slot->interface_number = params.iface_num;
  slot->trace_counts = 0xFFFF;
  slot->trace_length = 0xFFFF;
  slot->led_pending = keyboard;
  error = hid_host_device_start(handle);
  if (error != ESP_OK) {
    slot->handle = nullptr;
    slot->keyboard.store(false);
    slot->descriptor = KeyboardDescriptor{};
    hid_host_device_close(handle);
    ESP_LOGW(TAG, "Cannot start keyboard interface: %s", esp_err_to_name(error));
    return;
  }
  if (keyboard) input_count_.fetch_add(1);
  else auxiliary_count_.fetch_add(1);
  hid_host_dev_info_t info{};
  hid_host_get_device_info(handle, &info);
  ESP_LOGI(TAG, "%s %04X:%04X interface %u, report layouts: %u", keyboard ? "Keyboard" : "Auxiliary drain", info.VID, info.PID,
           params.iface_num, unsigned(slot->descriptor.layouts().size()));
}

void KVMUSBBridge::handle_input_(const Event &queued) {
  const auto handle = queued.handle;
  const auto event = queued.type;
  Slot *slot = nullptr;
  for (auto &entry : slots_) if (entry.handle == handle) { slot = &entry; break; }
  if (slot == nullptr) return;
  if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
    const bool keyboard = slot->keyboard.exchange(false);
    slot->handle = nullptr;
    slot->descriptor = KeyboardDescriptor{};
    if (keyboard) {
      input_count_.fetch_sub(1);
      recover_output_("Keyboard interface disconnected; held keys released", true);
    } else auxiliary_count_.fetch_sub(1);
    ControlRequest close;
    close.type = ControlType::CLOSE;
    close.handle = handle;
    queue_control_(close);
    ESP_LOGI(TAG, "%s interface disconnected", keyboard ? "Keyboard" : "Auxiliary");
  } else if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
    if (slot->trace_length != queued.length) {
      slot->trace_length = queued.length;
      trace_counts_(3, slot->interface_number, queued.length, queued.at_ms,
                    input_callbacks_.load(), millis() - queued.at_ms, reports_.size());
    }
    if (!report_is_fresh(millis(), queued.at_ms, input_boundary_valid_, input_boundary_ms_)) {
      stale_.fetch_add(1);
      if (!input_boundary_valid_ || int32_t(queued.at_ms - input_boundary_ms_) >= 0)
        recover_output_("Expired input transition; release-only recovery", true);
      return;
    }
    if (slot->descriptor.update(queued.data, queued.length)) {
      received_.fetch_add(1);
      const uint32_t age = millis() - queued.at_ms;
      if (age > max_input_age_ms_.load()) max_input_age_ms_.store(age);
      const auto counts = key_counts(slot->descriptor.state());
      if (counts != slot->trace_counts) {
        slot->trace_counts = counts;
        trace_counts_(0, slot->interface_number, counts, queued.at_ms, received_.load(), age, reports_.size());
      }
      recompute_(true);
    }
    else if (queued.length != 0) rejected_.fetch_add(1);
  } else if (event == HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR) {
    slot->descriptor.clear_state();
    if (slot->keyboard.load()) {
      recover_output_("Keyboard transfer error; restarting input endpoint", true);
      rejected_.fetch_add(1);
      input_errors_.fetch_add(1);
    } else auxiliary_errors_.fetch_add(1);
    // The IDF driver does NOT resubmit its interrupt transfer on this event.
    ControlRequest restart;
    restart.type = ControlType::RESTART;
    restart.handle = handle;
    queue_control_(restart);
  }
}

void KVMUSBBridge::recompute_(bool fresh_physical_report) {
  KeyState state;
  for (const auto &slot : slots_) if (slot.handle != nullptr && slot.keyboard.load()) state.merge(slot.descriptor.state());
  input_counts_.store(key_counts(state));
  release_gate_.observe(state, fresh_physical_report);
  waiting_for_release_.store(release_gate_.waiting());
  if (!release_gate_.waiting() && switch_hotkey_.observe(state, millis())) {
    // Drop unsent physical key states. The first macro packet releases the old
    // PC; the chord itself never reaches either desktop shortcut listener.
    reports_.clear();
    last_enqueued_ = BootReport{};
    ESP_LOGI(TAG, "Ctrl+Alt+/ consumed: requesting next KVM input");
  }
  const BootReport report = release_gate_.waiting() ? BootReport{} : switch_hotkey_.filtered_report(state);
  const auto counts = key_counts(report);
  desired_counts_.store(counts);
  if (counts != traced_desired_counts_) {
    traced_desired_counts_ = counts;
    trace_counts_(1, release_gate_.waiting() ? 1 : 0, counts, millis(), received_.load(), 0, reports_.size());
  }
  portENTER_CRITICAL(&state_mux_);
  desired_ = report;
  portEXIT_CRITICAL(&state_mux_);
  if (report == last_enqueued_) return;
  last_enqueued_ = report;
  if (!reports_.push(report, millis())) {
    overflows_.fetch_add(1);
    recover_output_("Output queue overflow; release-only recovery", true);
  }
}

BootReport KVMUSBBridge::current_report() {
  portENTER_CRITICAL(&state_mux_);
  const BootReport result = desired_;
  portEXIT_CRITICAL(&state_mux_);
  return result;
}

void KVMUSBBridge::service_output_() {
  const uint32_t now = millis();
  const uint32_t revision = output_revision_.load();
  const bool changed = revision != seen_output_revision_;
  if (changed) {
    seen_output_revision_ = revision;
    recover_output_("KVM USB lifecycle changed; stale keys and shortcut discarded");
    was_mounted_ = false;
  }
  if (detached_for_recovery_) {
    if (uint32_t(now - detach_started_ms_) >= 1000) {
      usb_serial_jtag_ll_phy_enable_pad(true);
      usb_serial_jtag_pull_override_vals_t pulls{};
      pulls.dp_pu = true;
      usb_serial_jtag_ll_phy_enable_pull_override(&pulls);
      usb_serial_jtag_ll_phy_disable_pull_override();
      tud_connect();
      detached_for_recovery_ = false;
      ESP_LOGI(TAG, "KVM keyboard output reattached after transfer stall");
    }
    mounted_.store(false);
    return;
  }
  const bool ready = tud_mounted() && !tud_suspended() && !awaiting_output_mount_.load();
  mounted_.store(ready);
  if (!ready) {
    if (was_mounted_) recover_output_("KVM USB unavailable; old transitions discarded");
    was_mounted_ = false;
    reports_.clear();
    release_gate_.arm();
    return;
  }
  if (!was_mounted_) {
    if (!changed) recover_output_("KVM USB ready; synchronizing released state");
    was_mounted_ = true;
    led_revision_.fetch_add(1);
  }
  if (output_failed_.exchange(false)) {
    recover_output_("KVM USB report failed; release-only recovery");
  }
  // A lost completion used to leave report_in_flight_ set forever, including
  // across a bus reset that completed between two worker iterations.
  if (report_in_flight_.load() && uint32_t(now - last_send_ms_) > 1000 &&
      (!have_reconnected_ || uint32_t(now - last_reconnect_ms_) >= 30000)) {
    reconnect_output_();
    return;
  }
  if (reports_.stale(now)) {
    stale_.fetch_add(reports_.size());
    recover_output_("KVM stopped draining reports; expired backlog discarded", true);
  }
  if (switch_hotkey_.active()) {
    if (switch_hotkey_.timed_out(now)) {
      recover_output_("USB hotkey sequence timed out; shortcut will not be replayed");
    } else {
      if (switch_hotkey_.waiting_for_ack() && !report_in_flight_.load() && switch_hotkey_.acknowledge(now)) {
        ESP_LOGI(TAG, "KVM next-input hotkey sequence delivered");
        recompute_(); // Re-arm immediately if the physical chord was already released.
      }
      if (switch_hotkey_.report_due(now) && send_report_(switch_hotkey_.next_report()))
        switch_hotkey_.submitted();
      return;
    }
  }
  // Respect SET_IDLE in units of 4ms, even when no key changes.
  const uint8_t idle = idle_rate_.load();
  if (reports_.empty() && idle != 0 && uint32_t(now - last_send_ms_) >= uint32_t(idle) * 4) {
    reports_.push(current_report(), now);
  }
  if (!reports_.empty()) {
    if (reports_.size() > max_output_depth_.load()) max_output_depth_.store(reports_.size());
    const auto age = reports_.age(now);
    if (age > max_output_wait_ms_.load()) max_output_wait_ms_.store(age);
    if (send_report_(reports_.front())) reports_.pop();
  }
}

bool KVMUSBBridge::send_report_(const BootReport &report) {
  // Wait for our completion callback, not only TinyUSB's endpoint busy flag:
  // its task can clear the latter before our callback runs on another core.
  if (report_in_flight_.load() || !tud_hid_ready()) return false;
  submitted_at_ms_.store(millis());
  report_in_flight_.store(true);
  if (!tud_hid_report(0, &report, sizeof(report))) {
    report_in_flight_.store(false);
    return false;
  }
  last_send_ms_ = millis();
  return true;
}

void KVMUSBBridge::report_delivered(const uint8_t *data, uint16_t length) {
  const uint32_t now = millis();
  const uint32_t age = now - submitted_at_ms_.load();
  if (age > max_ack_ms_.load()) max_ack_ms_.store(age);
  const uint32_t sequence = delivered_.fetch_add(1) + 1;
  if (data != nullptr && length == sizeof(BootReport)) {
    BootReport report;
    std::memcpy(&report, data, sizeof(report));
    const auto counts = key_counts(report);
    acked_counts_.store(counts);
    if (traced_ack_counts_.exchange(counts) != counts)
      trace_counts_(2, 0, counts, now, sequence, age, 0);
  }
  // Publish completion only after reading TinyUSB's report buffer.
  report_in_flight_.store(false);
}

void KVMUSBBridge::trace_counts_(uint8_t stage, uint8_t source, uint16_t counts,
                                uint32_t at, uint32_t sequence, uint32_t age, uint8_t queued) {
  // Optional diagnostics stop automatically after five minutes. Queueing a trace
  // never blocks either USB task; printing happens in the ESPHome main loop.
  if (count_traces_ == nullptr || millis() > 300000) return;
  const CountTrace trace{at, sequence, age, counts, stage, source, queued};
  if (xQueueSend(count_traces_, &trace, 0) != pdPASS) trace_drops_.fetch_add(1);
}

void KVMUSBBridge::abort_switch_(const char *reason) {
  if (!switch_hotkey_.active()) return;
  switch_hotkey_.abort();
  ESP_LOGW(TAG, "%s", reason);
}

void KVMUSBBridge::recover_output_(const char *reason, bool clear_inputs) {
  abort_switch_(reason);
  release_gate_.arm();
  waiting_for_release_.store(true);
  input_boundary_valid_ = true;
  input_boundary_ms_ = millis();
  if (clear_inputs) for (auto &slot : slots_) slot.descriptor.clear_state();
  reports_.release(input_boundary_ms_);
  last_enqueued_ = BootReport{};
  portENTER_CRITICAL(&state_mux_);
  desired_ = BootReport{};
  desired_counts_.store(0);
  portEXIT_CRITICAL(&state_mux_);
  recoveries_.fetch_add(1);
  ESP_LOGW(TAG, "%s", reason);
}

void KVMUSBBridge::output_event_(uint8_t event) {
  // Called in the TinyUSB task, in order with report-completion callbacks.
  // Mount is an epoch even when the worker never observed mounted == false.
  if (event == TINYUSB_EVENT_ATTACHED || event == TINYUSB_EVENT_DETACHED) {
    report_in_flight_.store(false);
    output_failed_.store(false);
  }
  if (event == TINYUSB_EVENT_ATTACHED) awaiting_output_mount_.store(false);
  output_revision_.fetch_add(1);
  const char *name = event == TINYUSB_EVENT_ATTACHED ? "attached" :
                     event == TINYUSB_EVENT_DETACHED ? "detached" :
                     event == TINYUSB_EVENT_SUSPENDED ? "suspended" : "resumed";
  ESP_LOGI(TAG, "KVM USB %s (epoch %u)", name, unsigned(output_revision_.load()));
}

void KVMUSBBridge::reconnect_output_() {
  recover_output_("KVM IN transfer stalled >1000ms; forcing keyboard re-enumeration", true);
  last_reconnect_ms_ = millis();
  have_reconnected_ = true;
  awaiting_output_mount_.store(true);
  tud_disconnect();
  usb_serial_jtag_pull_override_vals_t pulls{};
  usb_serial_jtag_ll_phy_enable_pull_override(&pulls);
  usb_serial_jtag_ll_phy_enable_pad(false);
  detach_started_ms_ = millis();
  detached_for_recovery_ = true;
  was_mounted_ = false;
}

void KVMUSBBridge::service_leds_() {
  const uint32_t revision = led_revision_.load();
  if (revision != seen_led_revision_) {
    seen_led_revision_ = revision;
    for (auto &slot : slots_) if (slot.handle != nullptr) slot.led_pending = true;
  }
  for (auto &slot : slots_) {
    if (!slot.led_pending || slot.handle == nullptr) continue;
    slot.led_pending = false;
    for (const auto &layout : slot.descriptor.layouts()) {
      auto report = slot.descriptor.led_report(layout, leds_.load());
      if (report.empty()) continue;
      if (report.size() > sizeof(ControlRequest::data)) {
        ESP_LOGD(TAG, "Skipping oversized optional LED report");
        continue;
      }
      // A receiver may take five seconds to time out a control request. Never
      // block the input/output worker while sending optional lock-key LEDs.
      if (uxQueueSpacesAvailable(controls_) <= 16) { slot.led_pending = true; break; }
      ControlRequest request;
      request.handle = slot.handle;
      request.type = ControlType::LED;
      request.report_id = layout.id;
      request.length = report.size();
      std::memcpy(request.data, report.data(), report.size());
      queue_control_(request);
    }
  }
}

void KVMUSBBridge::dump_config() {
  ESP_LOGCONFIG(TAG, "P4 USB keyboard bridge: HS host -> FS USB 1.1 boot 6KRO (8 bytes, no Report ID)");
  ESP_LOGCONFIG(TAG, "Input hub support: %s (board jumper selects direct socket OR three hub sockets)",
                host_hub_support_ ? "enabled, FS/LS only" : "disabled");
  ESP_LOGCONFIG(TAG, "Output: native USB-C GPIO24/25; logs: CH343/UART0");
  ESP_LOGCONFIG(TAG, "Ctrl+Alt+/ -> TESmart next input: %s", switch_hotkey_.enabled() ? "enabled" : "disabled");
}

void KVMUSBBridge::loop() {
  if (count_traces_ != nullptr) {
    CountTrace trace;
    for (unsigned i = 0; i < 16 && xQueueReceive(count_traces_, &trace, 0) == pdPASS; i++) {
      if (trace.stage == 3) {
        ESP_LOGI(TAG, "INPUT LENGTH t=%u interface=%u bytes=%u age=%ums", unsigned(trace.at_ms),
                 trace.source, trace.counts, unsigned(trace.age_ms));
        continue;
      }
      const char *stage = trace.stage == 0 ? "INPUT" : trace.stage == 1 ? "DESIRED" : "ACK";
      ESP_LOGI(TAG, "COUNT %s t=%u seq=%u source=%u held=%u mods=%u age=%ums queued=%u",
               stage, unsigned(trace.at_ms), unsigned(trace.sequence), trace.source,
               trace.counts & 0xFF, trace.counts >> 8, unsigned(trace.age_ms), trace.queued);
    }
  }
  if (worker_failed_.exchange(false)) {
    status_set_error(LOG_STR("USB worker stopped; check UART diagnostics"));
    mounted_.store(false);
  }
  if (millis() - last_status_ms_ > 10000) {
    last_status_ms_ = millis();
    ESP_LOGD(TAG, "Keyboard interfaces: %u, output mounted: %s, input reports: %u, delivered: %u, ignored: %u, overflows: %u",
             input_count_.load(), mounted_.load() ? "yes" : "no", received_.load(), delivered_.load(),
             rejected_.load(), overflows_.load());
    ESP_LOGD(TAG, "USB health: queued=%u in-flight-age=%ums recoveries=%u stale=%u input-errors=%u control-errors=%u max-control=%ums max-worker-gap=%ums",
             queued_output_.load(), in_flight_age_ms_.load(), recoveries_.load(), stale_.load(),
             input_errors_.load(), control_errors_.load(), max_control_ms_.load(), max_worker_gap_ms_.load());
    ESP_LOGD(TAG, "Key counts: input=%u/%u desired=%u/%u acked=%u/%u gate=%u; maxima: input-age=%ums queue=%u wait=%ums ACK=%ums trace-drops=%u",
             input_counts_.load() & 0xFF, input_counts_.load() >> 8,
             desired_counts_.load() & 0xFF, desired_counts_.load() >> 8,
             acked_counts_.load() & 0xFF, acked_counts_.load() >> 8, waiting_for_release_.load(),
             max_input_age_ms_.load(), max_output_depth_.load(), max_output_wait_ms_.load(), max_ack_ms_.load(), trace_drops_.load());
    ESP_LOGD(TAG, "Input packets: callbacks=%u empty=%u bytes8=%u bytes19=%u other=%u",
             input_callbacks_.load(), empty_input_.load(), array_input_.load(), bitmap_input_.load(), other_input_.load());
    ESP_LOGD(TAG, "Auxiliary drain: interfaces=%u reports=%u errors=%u", auxiliary_count_.load(),
             auxiliary_reports_.load(), auxiliary_errors_.load());
    ESP_LOGD(TAG, "USB state: HS root=%08X FS/LS-only=%u FS config=%08X status=%08X control=%08X PHY-swap=%u",
             host_enabled_ ? unsigned(USB_DWC_HS.hprt_reg.val) : 0u,
             host_enabled_ ? unsigned(USB_DWC_HS.hcfg_reg.fslssupp) : 0u, unsigned(USB_DWC_FS.dcfg_reg.val),
             unsigned(USB_DWC_FS.dsts_reg.val), unsigned(USB_DWC_FS.dctl_reg.val),
             unsigned(LP_SYS.usb_ctrl.sw_usb_phy_sel));
    ESP_LOGD(TAG, "FS pads: WRAP=%08X USJ=%08X PHY clocks=%08X USJ interrupts=%08X",
             unsigned(USB_WRAP.otg_conf.val), unsigned(USB_SERIAL_JTAG.conf0.val),
             unsigned(LP_AON_CLKRST.hp_usb_clkrst_ctrl0.val), unsigned(USB_SERIAL_JTAG.int_raw.val));
    ESP_LOGD(TAG, "FS init: IO24/25=%08X/%08X USJ-memory=%08X clocks1=%08X route=%08X",
             unsigned(REG_READ(IO_MUX_GPIO24_REG)), unsigned(REG_READ(IO_MUX_GPIO25_REG)),
             unsigned(REG_READ(USB_SERIAL_JTAG_MEM_CONF_REG)),
             unsigned(LP_AON_CLKRST.hp_usb_clkrst_ctrl1.val), unsigned(LP_SYS.usb_ctrl.val));
  }
}

void KVMUSBBridge::on_shutdown() {
  if (tud_hid_ready()) {
    BootReport empty;
    tud_hid_report(0, &empty, sizeof(empty));
  }
}

}  // namespace esphome::kvm_usb_bridge

extern "C" {
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
  ESP_LOGI("kvm_usb_bridge", "Host requested keyboard report descriptor (interface %u)", instance);
  return instance == 0 ? esphome::kvm_usb_bridge::REPORT_DESCRIPTOR : nullptr;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type,
                             uint8_t *buffer, uint16_t length) {
  using namespace esphome::kvm_usb_bridge;
  if (bridge_instance == nullptr || instance != 0 || report_id != 0) return 0;
  if (type == HID_REPORT_TYPE_OUTPUT && length != 0) { buffer[0] = bridge_instance->leds(); return 1; }
  if (type != HID_REPORT_TYPE_INPUT) return 0;
  const BootReport report = bridge_instance->current_report();
  const uint16_t count = std::min<uint16_t>(length, sizeof(report));
  std::memcpy(buffer, &report, count);
  return count;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type,
                         uint8_t const *buffer, uint16_t length) {
  using namespace esphome::kvm_usb_bridge;
  if (bridge_instance != nullptr && instance == 0 && report_id == 0 && type == HID_REPORT_TYPE_OUTPUT && length != 0)
    bridge_instance->set_leds(buffer[0]);
}

bool tud_hid_set_idle_cb(uint8_t instance, uint8_t idle_rate) {
  using namespace esphome::kvm_usb_bridge;
  if (bridge_instance == nullptr || instance != 0) return false;
  bridge_instance->set_idle(idle_rate);
  return true;
}

void tud_hid_set_protocol_cb(uint8_t instance, uint8_t protocol) {
  ESP_LOGI("kvm_usb_bridge", "Host selected %s protocol (interface %u)", protocol == 0 ? "boot" : "report", instance);
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t length) {
  using namespace esphome::kvm_usb_bridge;
  if (bridge_instance != nullptr && instance == 0) bridge_instance->report_delivered(report, length);
}

void tud_hid_report_failed_cb(uint8_t instance, hid_report_type_t type, uint8_t const *, uint16_t) {
  using namespace esphome::kvm_usb_bridge;
  if (bridge_instance != nullptr && instance == 0 && type == HID_REPORT_TYPE_INPUT)
    bridge_instance->report_failed();
}
}
