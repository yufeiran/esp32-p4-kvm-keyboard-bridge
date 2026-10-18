# Compatibility and evidence / 兼容性与验证证据

## Hardware result

Tested on 2026-10-18–19, with a Waveshare ESP32-P4-Module-DEV-KIT, production P4 rev3.2 and 16 MB flash.

| Keyboard / connection | Target | Result |
| --- | --- | --- |
| NuPhy Air75 V3 2.4 GHz receiver, `19F5:2620`, directly attached | TESmart HKS202-M24 dedicated keyboard port | Cannot type |
| Same receiver, directly attached | KVM general USB port | Can type |
| Same receiver through P4 bridge | PC USB port | Typing, Shift, Backspace, release tested working |
| Same receiver through P4 bridge | KVM dedicated keyboard port | Typing, Backspace and release tested working |
| Same bridge after OTA restart and CH343 hardware reset | Same KVM, output cable left connected | Automatically reconnects; typing tested working |
| Optional P4 Ctrl+Alt+/ macro | Same KVM, two attached computers | Switching both directions, post-switch typing/Backspace, and one switch per hold confirmed by the tester |
| Other keyboard models, wired Air75 V3, other KVMs/boards | — | Not validated by this project |

The hardware confirmations above used this bridge engine inside the original ESPHome installation, which also had unrelated network/control components. The independent `keyboard-bridge.yaml` removes those components and defaults to hotkey off. The v0.1.0 standalone factory image has also been flashed through CH343 and tested for typing, Shift, Backspace, release and restart recovery, without unplugging the KVM output. **A successful standalone compilation is not an additional hardware compatibility test.**

The same NuPhy receiver exposes both traditional and bitmap keyboard interfaces. No controlled NKRO-only / interface-only / USB-version-only comparison has been completed. Do not translate this matrix into “all NKRO keyboards fail” or “all 6KRO keyboards work.” The KVM controller chip and its exact firmware limitation remain unknown.

## Automated checks

- Traditional boot report: modifiers, padding, key arrays, LED encoding and release.
- 144-bit NKRO bitmap to 6KRO, seventh-key rollover and recovery.
- Report IDs, Push/Pop, consumer-usage isolation and interface merge.
- Error-state preservation, disconnect release, malformed/truncated input.
- Both actual Air75 V3 receiver descriptors and input report sizes.
- 10,000 deterministic randomized/malformed descriptor cases.
- Optional hotkey: disabled passthrough, exact seven-packet macro, completion/30 ms barriers, all six chord orders, left/right Ctrl/Alt, exclusion of Shift/GUI, held-key suppression, debounce, timeout/cancel without replay, timer wrap and early physical release.
- Linked-ELF audit: USB 1.10, one Boot Keyboard interface, HID 1.11, no Report ID, 8-byte IN endpoint and 1 ms interval.

The host tests exercise parser and state-machine behavior. They do not simulate USB bus scheduling, electrical behavior or the KVM's firmware.

## 中文说明

上表记录了**实际验证过的硬件组合**，不是全品牌兼容承诺。已验证的是 NuPhy Air75 V3 的 2.4G 接收器经过 P4 后，在 HKS202-M24 专用键盘口正常输入，并且重启后无需拔插恢复。可选热键的双向切换、切换后输入、退格和长按只切一次也由使用者确认。

这些硬件验证使用相同桥接核心的原集成固件；独立版删除网络等无关组件，默认关闭切换热键。v0.1.0 的独立工厂镜像也已经实际刷入，验证了打字、Shift、退格、松键和无需拔插的重启恢复。可选热键的实测来自同核心的集成配置。

**尚未证明 NKRO 是唯一根因**。没有隔离测试接口数量、Report ID、NKRO 和 USB 版本等变量，也未确认 KVM 主控型号。其他键盘、KVM 和 P4 开发板请提供实测结果。

## Reporting another combination

Use the GitHub compatibility issue template. Include exact board revision, keyboard/receiver/firmware, KVM, source commit, jumper/port arrangement and separate results for direct-to-PC, direct-to-KVM and through-P4. Include static descriptors or aggregate counters if useful. Do not upload typed reports, passwords or private network configuration.
