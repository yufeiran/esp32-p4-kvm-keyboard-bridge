# Implementation notes / 实现说明

Start with [what the bridge does and why](../README.md), or the [中文说明](../README.zh-CN.md). This page describes the implementation boundaries for contributors.

## USB roles and controller numbering

ESP32-P4 has separate HS and FS OTG controllers. The bridge uses the HS-capable controller as a host for the jumper-selected USB-A path and the FS controller as a device for the KVM. A keyboard does not have to operate at High Speed merely because it is attached to the HS controller.

In the pinned versions, **IDF Host numbers HS=0 / FS=1; TinyUSB numbers FS=0 / HS=1**. The host uses `peripheral_map = 1 << 0`; the device uses `TINYUSB_PORT_FULL_SPEED_0`. Reusing one stack's controller index in the other silently selects the wrong controller.

The tested board wires its native USB-C data pair to GPIO24/25. FS OTG's default PHY is on GPIO26/27. `usb_wrap_ll_phy_select` switches the route at runtime after TinyUSB installation. The CH343 is a separate UART bridge, so the console remains available on UART0 GPIO37/38.

The opt-in hub profile forces this input controller to FS/LS-only operation through a pinned-IDF compatibility shim. H3 selects one direct socket or three CH334 sockets, never all four. See [USB port topology and implementation](usb-ports.md).

## Start and restart

1. Enable the USB Serial/JTAG clock needed by native PHY0; leave the default OTG route while initializing the other PHY.
2. Override the native PHY pulls to off and disable its pads for one second. This detaches the boot ROM's USB device electrically.
3. Explicitly select active pad settings and drive capability on the output pins. Normal boot and ROM download mode leave different pad state.
4. Install TinyUSB FS, request a soft disconnect, route the controller to the desired PHY and reconnect after a short delay.
5. Restore the native pad/pull configuration for the selected route.

This sequence came from hardware debugging: OTG soft disconnect alone, or pad disable without removing the pull-up, did not reliably release the KVM's old USB connection after restart. A physical replug worked. Removing both pull-up and pads made software restart work too. It is board/SDK-specific code, not a general requirement for every USB bridge. eFuse configuration is read for diagnostics and **never written**.

## Input processing

The IDF HID driver notifies connection and input events. Callbacks copy each event/report into a bounded queue (64 entries) immediately. A forwarding worker handles parsing and output. A second serialized control task handles LED requests, interface cleanup and endpoint restart, so a slow optional control transfer cannot pause normal key forwarding. Initial interface opening/protocol selection still happens in the forwarding worker before streaming starts. Blocking HID control requests inside the driver's event task previously prevented progress; polling only the latest report also loses short press/release edges.

The parser in `hid_keyboard.h` is independent of ESPHome and FreeRTOS. It recognizes keyboard usage-page fields, array reports, bitmaps, Report IDs and global Push/Pop. It skips unrelated collections and validates descriptor/report bounds. It merges keyboard states within a pool of up to eight open HID interface slots. When `drain_auxiliary_interfaces` is enabled, non-keyboard interfaces also occupy slots and stay polled; their payloads are discarded directly in the callback and never enter the keyboard event queue. With optional media controls enabled, supported Consumer/System interfaces are instead parsed and forwarded, while vendor-only interfaces stay drain-only. Callback contexts refer to stable slots and use an atomic keyboard flag; only keyboard interfaces contribute held state or the connected-keyboard count. This is not a promise to support every possible HID descriptor: vendor-defined protocols, unusual layouts beyond parser bounds and non-keyboard usages need separate implementation.

The actual Air75 V3 fixture has a 62-byte descriptor for an 8-byte conventional keyboard report and a 57-byte descriptor for a 19-byte report (8 modifier bits plus a 144-bit bitmap). These descriptor bytes are protocol metadata; typed data is not included. Received reports are decoded according to their descriptor rather than being assumed to have one fixed offset.

Input disconnect clears that interface's held state. Malformed/truncated input cannot fabricate a release. Transfer errors clear stale held state and recover the retained descriptor. An input queue overflow clears the accumulated key state rather than retaining keys indefinitely.

## Output processing

By default, output is one HID Boot Keyboard interface, with no Report ID, an 8-byte input report and a one-byte LED output report. Both Boot and Report protocol use the same keyboard shape. Keyboard modifier bits are kept separately from the six ordinary usages. If more than six ordinary keys are held, the six output slots contain HID ErrorRollOver (`0x01`), then recover when the count drops.

A bounded timestamped output buffer preserves recent press/release transitions and drops old backlogs instead of replaying them on wake. Both input and output transitions have a 100 ms age bound. It waits for endpoint readiness and the bridge's own completion callback flag. TinyUSB can clear endpoint-busy before our callback runs on another core; the explicit in-flight flag avoids attributing an earlier completion to a later hotkey report. `SET_IDLE` is honored in four-millisecond units. LED reports travel back through each input descriptor's applicable LED layout; an unsupported receiver LED request is nonfatal.

Mount, suspend/resume, overflow and error recovery clear the old output buffer and begin with all keys released. A release gate requires a fresh physical all-up report before forwarding new key states, so a synthetic state clear does not re-arm stale held keys. Fast re-enumeration is detected through TinyUSB lifecycle epochs even if the worker never samples mounted=false. A one-second undelivered transfer on an active, nonsuspended output triggers a real PHY detach/reconnect, limited to one attempt per 30 seconds; shortcuts are not replayed. If input transfers fail, stop/start is requested through the serialized control task. Recovery failures are logged and counted; not every driver failure can be repaired automatically. There is no keystroke recording, networking or typed-character log in the standalone configuration. Optional descriptor dumps contain static HID metadata; INFO logs include connection state and explicit switch requests.

## Optional media interface

With `media_controls: true`, interface 0 remains Boot Keyboard with an 8-byte report and no Report ID; its ordinary usage range grows to `0x91`. A separate non-Boot interface 1 uses endpoint `0x82` (32-byte maximum, 1ms interval): Report ID 1 carries eight 16-bit Consumer usages (`0x0000–0x03FF`), and ID 2 carries three System Power/Sleep/Wake bits. Relative Consumer AC Pan and mouse reports are excluded. Each interface keeps independent completion state; a media endpoint failure does not reset the working keyboard endpoint. Media has a bounded timestamped queue and an independent physical-release gate. The gate quarantines controls known to be held at recovery; an idle recovery does not consume the next new encoder pulse. Repeated recovery or keyboard all-up cannot unlock an existing media quarantine. During a switching macro, media input is suppressed and released too. Optional count traces also record media input/desired/ACK timestamps and active-control counts; no usages or payloads are logged, and traces still stop after five minutes. No raw Fn key or vendor configuration protocol is forwarded. [Evidence and limitations](media-controls.md).

## Optional Ctrl+Alt+/ macro

`switch_hotkey.h` is a portable state machine, enabled only when configured. It recognizes Ctrl (either side), Alt (either side) and standard slash usage `0x38` becoming jointly held, regardless of the three arrival orders (all six permutations are tested). Shift/GUI combinations are excluded. Dropping an excluded modifier alone does not accidentally trigger a previously held chord.

The complete chord is consumed. States sent before the chord became complete cannot be retroactively hidden; the macro's first packet releases them. Unsent physical reports are discarded, and physical input is suppressed until the macro has ended and all physical keys have been released. This prevents a held Ctrl/Alt from following the user to the new computer.

The seven output reports are:

| Step | Report |
| --- | --- |
| 0 | All keys up |
| 1 | RightCtrl down (`modifiers=0x10`) |
| 2 | All keys up |
| 3 | RightCtrl down |
| 4 | All keys up |
| 5 | PageDown down (`usage=0x4E`) |
| 6 | All keys up |

Each step waits for USB transfer completion, then at least 30 ms before the next. An 800 ms trigger debounce and release latch prevent repeated switching from one hold. A two-second timeout, transfer failure or disconnect cancels the macro; it is not replayed when the KVM comes back. USB completion proves delivery to the USB host, not acceptance of a KVM command—physical host switching must also be tested.

This is the HKS202-M24's native next-input sequence with its default RightCtrl trigger. It does not use KVM UART or infer which computer is active. A differently configured trigger or another KVM requires changing the sequence deliberately.

## 中文要点

- P4 的 HS 控制器负责读取真实键盘，FS 控制器负责向 KVM 模拟键盘；两套驱动对控制器的编号不同。
- 输入通过 HID 描述符解析，而非硬编码 NuPhy 报告；把多个接口合并后重新生成单接口、无 Report ID 的 8 字节 6KRO 报告。
- USB 重启恢复涉及原生 PHY 的上拉、引脚和路由，单纯调用软断开不够；实现不写 eFuse。
- 回调只拷贝事件，工作任务负责控制请求和解析；输入输出都有队列，保留短促按下/松开，溢出时优先松键恢复。
- 可选热键把同时按的 Ctrl+Alt+/ 转成 KVM 要求的顺序按键，每一步等待传输完成，长按不重复、断线不重放。
- 成功转换并不证明 KVM 只支持 6KRO。报告形状、多接口、Report ID 等变量尚未逐项隔离。

## Primary references

- [ESP-IDF 5.5.4 USB Host](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32p4/api-reference/peripherals/usb_host.html)
- [Espressif USB Device](https://docs.espressif.com/projects/esp-usb/en/latest/esp32p4/usb_device.html)
- [USB-IF HID 1.11](https://www.usb.org/sites/default/files/hid1_11.pdf)
- [Waveshare board documentation and schematics](https://docs.waveshare.net/ESP32-P4-Module-DEV-KIT/)

Implementation-specific conclusions above come from this source tree and the recorded hardware tests, not from a claim about the KVM's internal firmware.

See [the current reliability investigation](reliability.md) for tested scope and remaining long-idle validation.
