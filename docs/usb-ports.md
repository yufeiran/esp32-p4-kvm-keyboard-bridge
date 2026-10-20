# USB input ports / USB 输入口

## What changes

`host_hub_support: true` enables one external USB hub and limits the HS-capable input controller to Full/Low Speed. The KVM-facing controller and 8-byte keyboard reports are unchanged. The direct-only profile remains the default. Build the optional offline profile with:

```sh
python -m esphome compile keyboard-bridge-hub.yaml
```

<!-- hardware-status -->
The tester confirmed typing and immediate Backspace release through each of the three CH334 sockets with the integrated b16d244 image. A route-change error/retry storm also caused a task-watchdog reset. The current integrated 0197521 image adds error-only restart pacing (100ms per serialized attempt) and prioritizes host cleanup above HID callbacks. DEVICE → HOST → DEVICE recovery, typing/release and bidirectional hotkey switching passed on this follow-up image. The offline hub profile compiled and passed its linked audit but was not flashed. Overnight stability is unverified. No four-onboard-socket compatibility is claimed.
<!-- /hardware-status -->

## Why four sockets are not four interchangeable ports

The Waveshare schematic connects CH334 downstream ports 1–3 to three USB-A sockets. Its fourth downstream pair (`USB4_P/N`) only reaches TP1/TP2 test points. The remaining USB-A socket (`USBHSD1_P/N`) connects to one branch of the FSUSB42 switch; the other branch connects the hub upstream (`USB_P/N`) to P4 (`USBD_P/N`). H3 selects the branch.

| H3 jumper position | Electrically connected path |
| --- | --- |
| HOST | One direct USB-A socket; bypasses CH334 |
| DEVICE side | CH334 upstream and its three USB-A sockets; direct socket disconnected |

The DEVICE label is the board's switch label: in this firmware P4 still acts as USB **host** on that path. Firmware cannot override H3's physical selection. For four interchangeable input ports, a separate four-port hub is an alternative. Activating all four onboard sockets would require a reviewed hardware modification to route the fourth socket to CH334 port 4. Neither is part of the software update.

## Host-stack implementation

With ESP-IDF 5.5.4, enabling hub support alone leaves an HS hub needing split transactions for a Full/Low-Speed keyboard; the pinned host stack does not support that path. Refusing High-Speed chirp makes the hub enumerate at Full Speed, avoiding Transaction Translation. The total input bus bandwidth is then 12 Mbit/s. This does not change the keyboard's report format or force an artificial maximum held-key duration.

ESP-USB 1.6.0 exposes `fsls_only`, but requires new HAL support absent from the pinned IDF 5.5.4. To keep the tested SDK, the optional profile wraps `hcd_port_command()` and sets `HCFG.FSLSSupp` immediately before every root-port RESET command, including resets after core recovery. Setting it at HAL initialization was tested and failed because the bit did not survive until enumeration. The original HCD command still runs. The bridge installs HCD only for controller 0 (HS input); controller 1 uses TinyUSB for FS KVM output and is untouched. No installed SDK files are patched. The shim declares the pinned private HCD ABI (`void*`, 32-bit command enum, RESET=2) and rejects every IDF version other than 5.5.4 at compile time and must be revalidated when changing the SDK. The direct-only build does not link this wrapper.

Only a single hub level is enabled. The existing pool has eight HID-interface slots, and auxiliary receiver interfaces consume slots too. Supporting three sockets does not promise three composite receivers simultaneously; hardware channels, power and interface count still limit concurrent devices. Mouse/media/vendor payloads are not forwarded.

## 中文

新增可选 `host_hub_support: true`，开启单层 Hub，并让输入端 HS 控制器按 Full/Low Speed 工作，避开当前驱动不支持的高速 Hub 转换路径。给 KVM 的 USB 输出、8 字节报告、松键恢复和切换热键不变。默认配置仍是直连；可用 `keyboard-bridge-hub.yaml` 构建离线 Hub 版本。三口功能已实测；恢复跟进版本的确切验证状态见本页英文段落及兼容性记录。

这块板不是四个口都接在 Hub 上：三个 USB-A 接 CH334 的第 1–3 路，第 4 路只引到 TP1/TP2。剩下一个 USB-A 是直连口，通过 FSUSB42 模拟开关与 Hub 上游二选一。H3 的 HOST 侧选择直连口，DEVICE 侧选择 Hub 的三个口。DEVICE 是板上丝印，使用本固件时 P4 仍是主机。软件无法绕过这个物理跳线；不改硬件不能让四个口同时接通。

本实现保留 ESP-IDF 5.5.4，不升级整套 SDK。可选模式通过链接包装器，在每次 HCD 根端口 RESET 命令之前设置 `HCFG.FSLSSupp`，包括核心恢复后的再次复位。曾实测初始化时设置无法保留到枚举，已改为复位前设置。FS 输出控制器不受影响，也不改本机 SDK 文件。包装器使用固定的私有 HCD ABI，编译时只接受 ESP-IDF 5.5.4，更换 SDK 必须重新验证。单层 Hub、八个 HID 接口槽和硬件通道仍有容量限制，不代表能同时接三只复合接收器。鼠标、多媒体和厂商数据仍不转发。

## Primary references

- [Waveshare board documentation and schematic](https://docs.waveshare.net/ESP32-P4-Module-DEV-KIT/)
- [ESP-USB Full/Low-Speed Only Host and hub limitations](https://docs.espressif.com/projects/esp-usb/en/latest/esp32p4/usb_host.html#full-low-speed-only-host)
- [ESP-IDF HAL FS/LS-only change](https://github.com/espressif/esp-idf/commit/72908ba37ddfad252bee2abf7c8ef1995cc86b3d)
- [ESP-USB 1.6.0 changelog](https://components.espressif.com/components/espressif/usb/versions/1.6.0/changelog)
