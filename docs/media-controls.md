# Media controls / 媒体按键与音量旋钮

## What works / 做成了什么

On 2026-10-26 the tester confirmed the NuPhy Air75 V3 receiver's volume knob, mute and Fn media actions through the Waveshare P4 and TESmart HKS202-M24 dedicated keyboard port. These controls previously did nothing because the bridge discarded non-keyboard usages. Ordinary typing also worked in the test. This is evidence for this combination, not all KVMs.

2026-10-26，使用者确认 NuPhy Air75 V3 接收器经过微雪 P4 和 TESmart HKS202-M24 专用键盘口时，音量旋钮、静音与 Fn 媒体动作恢复，普通打字也正常。此前这些非键盘用途码被桥接程序丢弃。此结果只覆盖实际测试的组合。

**Fn+Esc:** the tester also tried the receiver directly on the PC; this shortcut did not toggle F-key/media behavior there either. That behavior is therefore not introduced by P4. The keyboard normally resolves its own Fn layers before sending HID input; this project cannot infer a physical Fn press from an ordinary Esc report. Configure the keyboard's layers with its own tool when a different function-row layout is wanted. No alternate shortcut or keyboard firmware change was validated here.

**Fn+Esc：**相同接收器直连 PC 时也不能切换 F 区／媒体功能，因此不是 P4 引入的故障。Fn 通常在键盘内部选择层，发给 P4 的已经是处理后的 HID 动作；普通 Esc 报告不能证明物理 Fn 是否按下。需要不同的功能行布局时，应在键盘自己的配置工具中设置。这里没有验证另一组切换快捷键，也没有修改 NuPhy 固件。

## How it works / 原理

The input descriptor parser recognizes absolute Consumer arrays/bitmaps and Generic Desktop System Power/Sleep/Wake. It keeps Report IDs and keyboard state separate, and ignores the receiver's relative mouse report. Consumer controls get a new output interface instead of adding Report IDs to the primary keyboard packet.

输入解析器识别 Consumer 按键数组／位图，以及 Generic Desktop 的电源、休眠、唤醒控制；按 Report ID 分离状态，忽略接收器的相对鼠标报告。媒体动作通过新增接口输出，主键盘报告不会混入 Report ID。

| Property / 属性 | Optional media profile / 可选媒体配置 |
| --- | --- |
| Primary keyboard / 主键盘 | Interface 0, 8 bytes, six keys + modifiers, no Report ID / 接口 0，八字节，六键加修饰键，无 Report ID |
| Keyboard range / 普通键码 | `0x04–0x91`, includes F13–F24 / 包含 F13–F24 |
| Consumer output / 媒体输出 | Interface 1, Report ID 1, eight 16-bit usages up to `0x03FF` / 接口 1，八个 16 位用途码 |
| System output / 系统输出 | Same interface, Report ID 2, Power/Sleep/Wake bits / 同一接口，电源／休眠／唤醒三个位 |
| Control endpoint / 控制端点 | `0x82`, 32 bytes max, 1ms interval / 最大 32 字节，间隔 1ms |

Media has its own bounded queue, completion state and physical-release gate. Old transitions expire after 100ms. A media endpoint failure does not reset a working keyboard endpoint. Recovery may consume the first media action until its release arrives. Fn-layer switching, proprietary vendor commands, RGB configuration and mouse movement are not translated. An OS may ignore brightness or other usages depending on the selected computer/display.

媒体使用独立的有界队列、完成状态和物理松键门控，超过 100ms 的旧变化丢弃。媒体端点故障不会重置正常工作的键盘端点；恢复时可能消耗第一次媒体动作，收到松开后再按即可。Fn 层切换、厂商私有命令、RGB 配置及鼠标移动不在转换范围。亮度等功能是否生效还取决于当前电脑、操作系统和显示器。

## Implementation and tests / 实现与验证

The baseline `keyboard-bridge.yaml` keeps `media_controls: false` and its original single-interface descriptor. The opt-in example is offline, includes the hub profile and leaves the KVM switching shortcut disabled:

基础 `keyboard-bridge.yaml` 默认关闭媒体转发，保留原有单接口描述符。可选示例保持离线，包含 Hub 输入支持，默认关闭 KVM 切换热键：

```sh
python -m esphome compile keyboard-bridge-media.yaml
python tests/verify_firmware.py .esphome/build/p4-keyboard-bridge/.pioenvs/p4-keyboard-bridge/firmware.elf --hub --media
```

Use the same wiring and flashing procedure as the README. H3 DEVICE selects the three CH334 sockets; HOST selects the direct socket. The routes remain mutually exclusive. Other KVMs may ignore or reject the extra interface; use the keyboard-only profile in that case.

接线与烧录方式同 README。H3 DEVICE 选择三个 CH334 口，HOST 选择直连口，两条路径仍然互斥。其他 KVM 可能忽略或拒绝附加接口，需要单独实测。

Portable tests include the actual receiver's 123-byte interface-2 descriptor: mouse Report ID 2, System selector ID 3 and Consumer selector ID 4. Tests cover volume/mute/brightness, Report-ID isolation, truncated reports, control arrays/bitmaps, relative-mouse exclusion, independent release gates, queue aging, and optional extended keyboard output. Linked-ELF checks validate both interface descriptors, endpoints and Report IDs.

自动化测试加入接收器实际的 123 字节接口 2 描述符：鼠标 ID 2、System 选择器 ID 3、Consumer 选择器 ID 4。覆盖音量、静音、亮度、Report ID 隔离、截断报告、数组／位图、相对鼠标排除、独立松键门控、队列过期与扩展普通键。最终链接 ELF 另核对接口、端点及 Report ID。

The first hardware-confirmed integrated application was SHA-256 `09495afe9eedfc10defa8c344372fc29df57868b8631c63281c8c228c87b9146` (2026-10-26 21:00:05 +08:00). Its keyboard range still ended at `0x65`; F13–F24 were added afterward. Integrated firmware includes unrelated private components; standalone builds must be distinguished from that hardware test. The original v0.1.0 prebuilt binary does not contain media support.

首个硬件确认的集成应用 SHA-256 为上述 `09495afe…`（2026-10-26 21:00:05，UTC+8），当时普通键仍到 `0x65`，F13–F24 是随后补充的。集成固件还有无关的私人组件，不能把这次实测直接写成独立配置也已刷入测试。原 v0.1.0 预编译镜像不包含媒体支持。

Static descriptors and aggregate counters are sufficient for diagnostics. Typed reports, characters and private configuration are not included in this repository.

诊断仅使用静态描述符和汇总计数，仓库不包含输入报告、文字或私人配置。


### Final integrated regression / 最终集成版本回归

Application SHA-256 `128473103899f2bb9efb94bbd272c51de2dc490e3452eda37e7489e6144d8058`, built 2026-10-26 21:19:42 +08:00, was uploaded by OTA. Successful boot and KVM output mounting were observed. The tester confirmed ordinary typing, immediate held-Backspace release, knob volume/mute, Fn media actions and bidirectional Ctrl+Alt+/ switching on this final image. It includes the extended ordinary usage range; individual F13–F24 and System Power/Sleep/Wake actions were not physically tested. Overnight stability remains unverified.

最终应用 SHA-256 如上，构建于 2026-10-26 21:19:42（UTC+8），OTA 后已观察到成功启动确认与 KVM 输出挂载。使用者确认普通输入、长按退格松手即停、旋钮音量／静音、Fn 媒体动作和双向 Ctrl+Alt+/ 切换全部正常。此镜像包含扩展普通键码范围；没有单独物理验证 F13–F24，也未触发电源／休眠／唤醒。整夜稳定性仍未验证。

### Standalone build / 独立配置构建

The offline `keyboard-bridge-media.yaml` profile built successfully at 2026-10-26 21:23:49 +08:00. Application SHA-256: `a4a9959094ab26bf7c9b9c22bf81476120e9618696fae72e496577f3cd40b4a0`. The linked `--hub --media` audit passed, including both HID interfaces and the compiled HCD ABI/reset wrapper. This standalone image was **not flashed** during this session. The [CI run for the implementation commit](https://github.com/yufeiran/esp32-p4-kvm-keyboard-bridge/actions/runs/40330742149) also runs sanitized portable tests and builds/audits all three example profiles; consult that run for its outcome.

离线 `keyboard-bridge-media.yaml` 已于上述时间成功构建，应用 SHA-256 如上；`--hub --media` 最终链接检查通过，包含两个 HID 接口、HCD ABI 和复位包装器。此次**没有刷入独立镜像**。实现提交的 CI 同时运行带内存检查的测试及三种示例的构建／核验，状态以所链接的运行记录为准。
