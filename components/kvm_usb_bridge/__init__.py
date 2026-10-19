"""P4 dual-controller USB keyboard bridge (HS host, FS boot keyboard)."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import final_validate as fv
from esphome.components import esp32
from esphome.const import CONF_ID

DEPENDENCIES = ["esp32"]
CONFLICTS_WITH = ["tinyusb", "usb_host", "usb_cdc_acm"]
AUTO_LOAD = ["binary_sensor", "sensor"]

ns = cg.esphome_ns.namespace("kvm_usb_bridge")
KVMUSBBridge = ns.class_("KVMUSBBridge", cg.Component)

CONFIG_SCHEMA = cv.All(
    cv.Schema({
        cv.GenerateID(): cv.declare_id(KVMUSBBridge),
        cv.Optional("host_enabled", default=True): cv.boolean,
        cv.Optional("output_phy", default="GPIO24_GPIO25"): cv.one_of("GPIO24_GPIO25", "GPIO26_GPIO27"),
        cv.Optional("swap_data_pins", default=False): cv.boolean,
        cv.Optional("dump_descriptors", default=False): cv.boolean,
        cv.Optional("switch_hotkey", default=False): cv.boolean,
    }).extend(cv.COMPONENT_SCHEMA),
    esp32.only_on_variant(supported=[esp32.VARIANT_ESP32P4]),
)


def _validate(config):
    if fv.full_config.get()["esp32"]["framework"]["type"] != "esp-idf":
        raise cv.Invalid("kvm_usb_bridge requires framework.type: esp-idf")
    logger = fv.full_config.get().get("logger", {})
    if logger.get("hardware_uart") in ("USB_CDC", "USB_SERIAL_JTAG"):
        raise cv.Invalid("kvm_usb_bridge needs the hardware UART logger (UART0 / CH343)")
    return config


FINAL_VALIDATE_SCHEMA = _validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_host_enabled(config["host_enabled"]))
    cg.add(var.set_output_phy(0 if config["output_phy"] == "GPIO24_GPIO25" else 1))
    cg.add(var.set_swap_data_pins(config["swap_data_pins"]))
    cg.add(var.set_dump_descriptors(config["dump_descriptors"]))
    cg.add(var.set_switch_hotkey(config["switch_hotkey"]))
    esp32.add_idf_component(name="espressif/esp_tinyusb", ref="2.1.1")
    esp32.add_idf_component(name="espressif/usb_host_hid", ref="1.1.0")
    esp32.include_builtin_idf_component("usb")
    esp32.add_idf_sdkconfig_option("CONFIG_TINYUSB_HID_ENABLED", True)
    esp32.add_idf_sdkconfig_option("CONFIG_TINYUSB_HID_COUNT", 1)
    esp32.add_idf_sdkconfig_option("CONFIG_TINYUSB_HID_BUFSIZE", 64)
    esp32.add_idf_sdkconfig_option("CONFIG_TINYUSB_SUSPEND_CALLBACK", True)
    esp32.add_idf_sdkconfig_option("CONFIG_TINYUSB_RESUME_CALLBACK", True)
    esp32.add_idf_sdkconfig_option("CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE", 2048)
    # Direct HS root port only: bypass Waveshare's CH334 hub with the HOST jumper.
    esp32.add_idf_sdkconfig_option("CONFIG_USB_HOST_HUBS_SUPPORTED", False)
    # ESPHome 2026.5's logger references USJ VFS symbols even when UART0 is selected.
    # Keep that code linked; the actual console stays on CH343/UART0. PHY routing
    # is switched after TinyUSB installation, moving USJ to unused GPIO26/27.
    esp32.add_idf_sdkconfig_option("CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG", True)
    esp32.add_idf_sdkconfig_option("CONFIG_ESP_CONSOLE_SECONDARY_NONE", False)
