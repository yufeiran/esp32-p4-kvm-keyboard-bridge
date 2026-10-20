// ESP-IDF 5.5.4 compatibility shim for FS/LS-only host operation.
// Apply HCFG.FSLSSupp immediately before EVERY root-port reset, matching
// Espressif's newer usb_dwc_hal_port_toggle_reset() implementation.
// No SDK files are modified; the direct profile does not link this wrapper.
#include "esphome/core/defines.h"
#ifdef USE_KVM_USB_HOST_HUB
#include "esp_idf_version.h"
#include "esp_err.h"
#include "hal/usb_dwc_ll.h"

#if ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(5, 5, 4)
#error "Hub reset shim requires ESP-IDF 5.5.4; revalidate HCD ABI before upgrading"
#endif

// Pinned private HCD ABI: hcd_port_handle_t is void*, hcd_port_cmd_t is an
// unpacked enum (int), and HCD_PORT_CMD_RESET == 2. Do not use short enums.
// The bridge installs HCD for controller 0 only; FS output uses TinyUSB.
extern "C" esp_err_t __real_hcd_port_command(void *port, int command);
extern "C" esp_err_t __wrap_hcd_port_command(void *port, int command) {
  constexpr int ROOT_PORT_RESET = 2;
  if (command == ROOT_PORT_RESET)
    usb_dwc_ll_hcfg_set_fsls_supp_only(USB_DWC_LL_GET_HW(0));
  return __real_hcd_port_command(port, command);
}
#endif
