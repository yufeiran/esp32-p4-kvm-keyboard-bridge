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
  reports_ = xQueueCreate(64, sizeof(BootReport));
  events_ = xQueueCreate(64, sizeof(Event));
  if (reports_ == nullptr || events_ == nullptr) { mark_failed(); return; }

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
  host.peripheral_map = 1u << 0;  // HS root controller, physically the direct USB-A port.
  error = usb_host_install(&host);
  if (error != ESP_OK) {
    ESP_LOGE(TAG, "HS host initialization failed: %s", esp_err_to_name(error));
    mark_failed();
    return;
  }
  if (xTaskCreate(host_daemon_, "kvm_usb_host", 4096, this, 4, nullptr) != pdPASS) {
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
  if (xTaskCreate(bridge_task_, "kvm_usb_bridge", 8192, this, 5, nullptr) != pdPASS) {
    mark_failed(); return;
  }
  ESP_LOGI(TAG, "Ready: USB-A direct host -> FS boot keyboard on native USB-C (GPIO24/25)");
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
    // Blocking HID control requests must run outside the driver's event task.
    // Copy each input report in its callback; fetching "latest" here loses edges.
    Event event;
    const bool overflow = self->input_overflow_.exchange(false);
    for (unsigned count = 0; count < 64 && xQueueReceive(self->events_, &event, 0) == pdPASS; count++) {
      if (event.connected) self->open_keyboard_(event.handle);
      else if (!overflow || event.type != HID_HOST_INTERFACE_EVENT_INPUT_REPORT) self->handle_input_(event);
    }
    if (overflow) {
      for (auto &slot : self->slots_) slot.descriptor.clear_state();
      self->recompute_();
    }
    self->service_output_();
    self->service_leds_();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  // Best effort release if a worker fails; leave the ESPHome runtime alive for diagnostics.
  if (tud_hid_ready()) {
    BootReport empty;
    tud_hid_report(0, &empty, sizeof(empty));
  }
  vTaskDelete(nullptr);
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
  auto *self = static_cast<KVMUSBBridge *>(arg);
  Event queued;
  queued.handle = handle;
  queued.type = event;
  if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT &&
      hid_host_device_get_raw_input_report_data(handle, queued.data, sizeof(queued.data), &queued.length) != ESP_OK) {
    self->rejected_.fetch_add(1);
    return;
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
  hid_host_device_config_t config{};
  config.callback = interface_event_;
  config.callback_arg = this;
  esp_err_t error = hid_host_device_open(handle, &config);
  if (error != ESP_OK) {
    ESP_LOGW(TAG, "Cannot open HID interface %u: %s", params.iface_num, esp_err_to_name(error));
    return;
  }
  size_t length = 0;
  uint8_t *descriptor = hid_host_get_report_descriptor(handle, &length);
  Slot *slot = nullptr;
  for (auto &entry : slots_) if (entry.handle == nullptr) { slot = &entry; break; }
  if (slot == nullptr || descriptor == nullptr || length > 2048 || !slot->descriptor.parse(descriptor, length)) {
    ESP_LOGD(TAG, "Skipping non-keyboard/unsupported HID interface %u", params.iface_num);
    hid_host_device_close(handle);
    return;
  }
  if (dump_descriptors_) {
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
  if (params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
    error = hid_class_request_set_protocol(handle, HID_REPORT_PROTOCOL_REPORT);
    if (error != ESP_OK) {
      ESP_LOGW(TAG, "Report protocol request failed on interface %u: %s", params.iface_num, esp_err_to_name(error));
      hid_host_device_close(handle);
      return;
    }
  }
  slot->handle = handle;
  slot->led_pending = true;
  error = hid_host_device_start(handle);
  if (error != ESP_OK) {
    slot->handle = nullptr;
    slot->descriptor = KeyboardDescriptor{};
    hid_host_device_close(handle);
    ESP_LOGW(TAG, "Cannot start keyboard interface: %s", esp_err_to_name(error));
    return;
  }
  input_count_.fetch_add(1);
  hid_host_dev_info_t info{};
  hid_host_get_device_info(handle, &info);
  ESP_LOGI(TAG, "Keyboard %04X:%04X interface %u, report layouts: %u", info.VID, info.PID,
           params.iface_num, unsigned(slot->descriptor.layouts().size()));
}

void KVMUSBBridge::handle_input_(const Event &queued) {
  const auto handle = queued.handle;
  const auto event = queued.type;
  Slot *slot = nullptr;
  for (auto &entry : slots_) if (entry.handle == handle) { slot = &entry; break; }
  if (slot == nullptr) return;
  if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
    slot->handle = nullptr;
    slot->descriptor = KeyboardDescriptor{};
    input_count_.fetch_sub(1);
    recompute_();
    hid_host_device_close(handle);
    ESP_LOGI(TAG, "Keyboard interface disconnected; held keys released");
  } else if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
    if (slot->descriptor.update(queued.data, queued.length)) {
      received_.fetch_add(1);
      recompute_();
    }
    else if (queued.length != 0) rejected_.fetch_add(1);
  } else if (event == HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR) {
    slot->descriptor = KeyboardDescriptor{};
    recompute_();
    rejected_.fetch_add(1);
    // A descriptor is retained by the driver; recover parsing without retaining held keys.
    size_t length = 0;
    auto *descriptor = hid_host_get_report_descriptor(handle, &length);
    if (descriptor != nullptr) slot->descriptor.parse(descriptor, length);
    ESP_LOGW(TAG, "Keyboard input transfer error; held state cleared");
  }
}

void KVMUSBBridge::recompute_() {
  KeyState state;
  for (const auto &slot : slots_) if (slot.handle != nullptr) state.merge(slot.descriptor.state());
  if (switch_hotkey_.observe(state, millis())) {
    // Drop unsent physical key states. The first macro packet releases the old
    // PC; the chord itself never reaches either desktop shortcut listener.
    xQueueReset(reports_);
    pending_valid_ = false;
    last_enqueued_ = BootReport{};
    ESP_LOGI(TAG, "Ctrl+Alt+/ consumed: requesting next KVM input");
  }
  const BootReport report = switch_hotkey_.filtered_report(state);
  portENTER_CRITICAL(&state_mux_);
  desired_ = report;
  portEXIT_CRITICAL(&state_mux_);
  if (report == last_enqueued_) return;
  last_enqueued_ = report;
  if (xQueueSend(reports_, &report, 0) != pdPASS) {
    overflows_.fetch_add(1);
    xQueueReset(reports_);
    BootReport empty;
    xQueueSend(reports_, &empty, 0);
    xQueueSend(reports_, &report, 0);
  }
}

BootReport KVMUSBBridge::current_report() {
  portENTER_CRITICAL(&state_mux_);
  const BootReport result = desired_;
  portEXIT_CRITICAL(&state_mux_);
  return result;
}

void KVMUSBBridge::service_output_() {
  const bool mounted = tud_mounted();
  mounted_.store(mounted);
  if (!mounted) {
    abort_switch_("USB output disconnected; shortcut will not be replayed");
    was_mounted_ = false;
    pending_valid_ = false;
    report_in_flight_.store(false);
    xQueueReset(reports_);
    return;
  }
  if (!was_mounted_) {
    was_mounted_ = true;
    report_in_flight_.store(false);
    xQueueReset(reports_);
    pending_ = BootReport{};
    pending_valid_ = true;
    const BootReport current = current_report();
    xQueueSend(reports_, &current, 0);
    last_enqueued_ = current;
    led_revision_.fetch_add(1);
  }
  if (output_failed_.exchange(false)) {
    abort_switch_("USB report failed; shortcut canceled");
    xQueueReset(reports_);
    pending_ = BootReport{};
    pending_valid_ = true;
    const BootReport current = current_report();
    xQueueSend(reports_, &current, 0);
    last_enqueued_ = current;
  }
  if (switch_hotkey_.active()) {
    const uint32_t now = millis();
    if (switch_hotkey_.timed_out(now)) {
      abort_switch_("USB hotkey sequence timed out; shortcut will not be replayed");
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
  if (!pending_valid_) pending_valid_ = xQueueReceive(reports_, &pending_, 0) == pdPASS;
  // Respect SET_IDLE in units of 4ms, even when no key changes.
  const uint8_t idle = idle_rate_.load();
  if (!pending_valid_ && idle != 0 && millis() - last_send_ms_ >= uint32_t(idle) * 4) {
    pending_ = current_report();
    pending_valid_ = true;
  }
  if (pending_valid_ && send_report_(pending_)) {
    pending_valid_ = false;
  }
}

bool KVMUSBBridge::send_report_(const BootReport &report) {
  // Wait for our completion callback, not only TinyUSB's endpoint busy flag:
  // its task can clear the latter before our callback runs on another core.
  if (report_in_flight_.load() || !tud_hid_ready()) return false;
  report_in_flight_.store(true);
  if (!tud_hid_report(0, &report, sizeof(report))) {
    report_in_flight_.store(false);
    return false;
  }
  last_send_ms_ = millis();
  return true;
}

void KVMUSBBridge::abort_switch_(const char *reason) {
  if (!switch_hotkey_.active()) return;
  switch_hotkey_.abort();
  xQueueReset(reports_);
  pending_ = BootReport{};
  pending_valid_ = true;
  last_enqueued_ = BootReport{};
  recompute_();
  ESP_LOGW(TAG, "%s", reason);
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
      const esp_err_t error = hid_class_request_set_report(slot.handle, HOST_HID_REPORT_TYPE_OUTPUT,
                                                          layout.id, report.data(), report.size());
      if (error != ESP_OK) ESP_LOGD(TAG, "Keyboard LED report unsupported: %s", esp_err_to_name(error));
    }
  }
}

void KVMUSBBridge::dump_config() {
  ESP_LOGCONFIG(TAG, "P4 USB keyboard bridge: HS host -> FS USB 1.1 boot 6KRO (8 bytes, no Report ID)");
  ESP_LOGCONFIG(TAG, "Input: direct USB-A, HOST jumper; Output: native USB-C GPIO24/25; logs: CH343/UART0");
  ESP_LOGCONFIG(TAG, "Ctrl+Alt+/ -> TESmart next input: %s", switch_hotkey_.enabled() ? "enabled" : "disabled");
}

void KVMUSBBridge::loop() {
  if (worker_failed_.exchange(false)) {
    status_set_error(LOG_STR("USB worker stopped; check UART diagnostics"));
    mounted_.store(false);
  }
  if (millis() - last_status_ms_ > 10000) {
    last_status_ms_ = millis();
    ESP_LOGD(TAG, "Keyboard interfaces: %u, output mounted: %s, input reports: %u, delivered: %u, ignored: %u, overflows: %u",
             input_count_.load(), mounted_.load() ? "yes" : "no", received_.load(), delivered_.load(),
             rejected_.load(), overflows_.load());
    ESP_LOGD(TAG, "USB state: HS root=%08X FS config=%08X status=%08X control=%08X PHY-swap=%u",
             host_enabled_ ? unsigned(USB_DWC_HS.hprt_reg.val) : 0u, unsigned(USB_DWC_FS.dcfg_reg.val),
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

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *, uint16_t) {
  using namespace esphome::kvm_usb_bridge;
  if (bridge_instance != nullptr && instance == 0) bridge_instance->report_delivered();
}

void tud_hid_report_failed_cb(uint8_t instance, hid_report_type_t type, uint8_t const *, uint16_t) {
  using namespace esphome::kvm_usb_bridge;
  if (bridge_instance != nullptr && instance == 0 && type == HID_REPORT_TYPE_INPUT)
    bridge_instance->report_failed();
}
}
