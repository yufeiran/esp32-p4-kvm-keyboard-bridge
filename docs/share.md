# A short explanation to share / 可直接分享的说明

## 中文

做了一个 ESP32-P4 USB 键盘兼容桥：原本 NuPhy Air75 V3 的 2.4G 接收器接 TESmart HKS202-M24 专用键盘口完全不能打字，经 P4 转换后可以正常输入、使用修饰键和退格，重启也能自动重连。

原理是让 P4 一端作为 USB Host 读取键盘 HID 报告，另一端作为单接口、无 Report ID 的标准 8 字节 6KRO Boot Keyboard 连接 KVM。转换在 P4 上完成，不用改 NuPhy 固件或安装电脑端程序；可选 Ctrl+Alt+/ 还能转成 KVM 自己的切换按键序列。

这说明通过重新呈现简单 HID 键盘可以绕过当前的兼容问题；还不能单凭此结果断定 KVM 只支持 6KRO，或问题一定是 USB 1.1 / NKRO。源码、接线、测试与中英文说明已公开：

https://github.com/yufeiran/esp32-p4-kvm-keyboard-bridge

后续增加了可选媒体接口，Air75 V3 的旋钮音量、静音及 Fn 媒体动作已经在同一 KVM 专用口实测恢复；主键盘仍使用原有八字节格式。详见[媒体实现与验证](media-controls.md)。

## English

I built an ESP32-P4 USB keyboard compatibility bridge. A NuPhy Air75 V3 2.4 GHz receiver that could not type through a TESmart HKS202-M24 dedicated keyboard port now works through the bridge, including modifiers, Backspace and automatic reconnection after restart.

P4 hosts the original keyboard, decodes and merges its HID reports, then presents a single-interface, no-Report-ID, eight-byte 6KRO Boot Keyboard to the KVM. No keyboard firmware changes or desktop helper are required. An optional simultaneous Ctrl+Alt+/ chord becomes the KVM's native sequential next-input shortcut.

This demonstrates a working HID compatibility workaround. It does not isolate the KVM's exact limitation or prove that NKRO or USB 1.1 alone caused the original failure. Source, wiring, tests and bilingual documentation:

https://github.com/yufeiran/esp32-p4-kvm-keyboard-bridge

An optional media interface now restores the Air75 V3 volume knob, mute and Fn media actions through the same dedicated KVM port, with the primary keyboard's eight-byte format retained. See [implementation and evidence](media-controls.md).
