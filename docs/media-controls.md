# Media controls / 媒体按键与音量旋钮

## What works / 做成了什么

On 2026-10-26 the tester confirmed the NuPhy Air75 V3 receiver's volume knob, mute and Fn media actions through the Waveshare P4 and TESmart HKS202-M24 dedicated keyboard port. These controls previously did nothing because the bridge discarded non-keyboard usages. Ordinary typing also worked in the test. This is evidence for this combination, not all KVMs.

2026-10-26，使用者确认 NuPhy Air75 V3 接收器经过微雪 P4 和 TESmart HKS202-M24 专用键盘口时，音量旋钮、静音与 Fn 媒体动作恢复，普通打字也正常。此前这些非键盘用途码被桥接程序丢弃。此结果只覆盖实际测试的组合。

**Function-row toggle:** the tester found that **Fn+X+F** toggles F-key/media behavior on this Air75 V3. Fn+Esc did not toggle it when connected directly to the PC either. The keyboard resolves its own Fn layers before sending HID input; this project cannot infer a physical Fn press from an ordinary Esc report. No NuPhy firmware or keymap was changed by this project.

**功能行切换：**使用者已找到这把 Air75 V3 的切换方式为 **Fn+X+F**。Fn+Esc 直连 PC 也无效。Fn 在键盘内部选择层，发给 P4 的已经是处理后的 HID 动作；普通 Esc 报告不能证明物理 Fn 是否按下。本项目没有修改 NuPhy 固件或键位配置。

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

Media has its own bounded queue, completion state and physical-release gate. Old transitions expire after 100ms. A media endpoint failure does not reset a working keyboard endpoint. Controls held at recovery remain quarantined until a fresh physical release; idle recovery allows the first new encoder pulse through. Fn-layer switching, proprietary vendor commands, RGB configuration and mouse movement are not translated. An OS may ignore brightness or other usages depending on the selected computer/display.

媒体使用独立的有界队列、完成状态和物理松键门控，超过 100ms 的旧变化丢弃。媒体端点故障不会重置正常工作的键盘端点；恢复时仍按住的媒体键必须先真实松开，空闲状态恢复则直接接受下一次新旋钮动作。Fn 层切换、厂商私有命令、RGB 配置及鼠标移动不在转换范围。亮度等功能是否生效还取决于当前电脑、操作系统和显示器。

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

### First encoder action after recovery / 恢复后的第一格

A follow-up report described an unresponsive first detent, sometimes requiring a longer turn, after inactivity on both Windows and macOS. Code review identified a definite bridge-side defect: every media recovery armed the release gate even when no media control was held. The next nonzero input was consequently suppressed until its release. Current source arms the quarantine only for controls known to be held, preserves an existing quarantine across repeated recovery, and still requires a real media release to clear it. Portable tests reproduce the old first-pulse loss and cover the new idle/held cases. This fix does **not** prove the cause of every reported idle delay.

后续反馈是在 Windows 和 macOS 闲置后，旋钮第一格无效，有时要转较多才响应。代码检查确认桥接中存在一个明确缺陷：每次媒体恢复都强制等待一次松开，即使当时没有媒体键按下，也会吞掉下一个非零动作。当前源码只隔离恢复时确实按住的媒体键，重复恢复不会解除已有隔离，解除仍要求真实媒体松开。自动化测试覆盖了旧逻辑吞首格以及新的空闲／按住情况。此修复**不能单独证明所有闲置延迟的原因**。

Optional count traces show media input, desired output and USB completion timestamps, active-control counts and queue depth. Aggregate counters include gate suppression and input-pulse widths. They record neither usages nor payloads. USB completion confirms that the KVM received a transfer; it does not prove that the KVM forwarded that action to the selected computer.

可选计数诊断记录媒体输入、期望输出、USB 完成的时间、活动控制数量和队列深度，汇总计数还包含门控拦截次数及输入脉冲宽度，不记录用途码或原始内容。USB 完成只证明 KVM 接收了传输，不证明它已把动作转到当前电脑。

The [official Air75 V3 quick guide](https://www.nuphy.io/ali-oss/image/2025/07/10/469a9eceb212498ea8bc42e4f62ec877.png) documents automatic sleep after six minutes and Fn+] to toggle sleep (three green flashes: enabled; three red flashes: disabled). A direct-PC idle comparison and a temporary sleep-disabled comparison are useful controls; sleep has not been established as this report's cause.

[Air75 V3 官方快速指南](https://www.nuphy.io/ali-oss/image/2025/07/10/469a9eceb212498ea8bc42e4f62ec877.png)标注约六分钟无操作后休眠，Fn+] 切换休眠开关（绿灯三闪：开启；红灯三闪：关闭）。直连电脑的闲置对照，以及临时关闭休眠后的对照，有助于区分无线唤醒与桥接延迟；这里尚未证明休眠就是本次原因。


### Final integrated regression / 最终集成版本回归

Application SHA-256 `128473103899f2bb9efb94bbd272c51de2dc490e3452eda37e7489e6144d8058`, built 2026-10-26 21:19:42 +08:00, was uploaded by OTA. Successful boot and KVM output mounting were observed. The tester confirmed ordinary typing, immediate held-Backspace release, knob volume/mute, Fn media actions and bidirectional Ctrl+Alt+/ switching on this final image. It includes the extended ordinary usage range; individual F13–F24 and System Power/Sleep/Wake actions were not physically tested. Overnight stability remains unverified.

最终应用 SHA-256 如上，构建于 2026-10-26 21:19:42（UTC+8），OTA 后已观察到成功启动确认与 KVM 输出挂载。使用者确认普通输入、长按退格松手即停、旋钮音量／静音、Fn 媒体动作和双向 Ctrl+Alt+/ 切换全部正常。此镜像包含扩展普通键码范围；没有单独物理验证 F13–F24，也未触发电源／休眠／唤醒。整夜稳定性仍未验证。

### Standalone build / 独立配置构建

The offline `keyboard-bridge-media.yaml` profile built successfully at 2026-10-26 21:23:49 +08:00. Application SHA-256: `a4a9959094ab26bf7c9b9c22bf81476120e9618696fae72e496577f3cd40b4a0`. The linked `--hub --media` audit passed, including both HID interfaces and the compiled HCD ABI/reset wrapper. This standalone image was **not flashed** during this session. The [CI run for the implementation commit](https://github.com/yufeiran/esp32-p4-kvm-keyboard-bridge/actions/runs/40330742149) also runs sanitized portable tests and builds/audits all three example profiles; consult that run for its outcome.

离线 `keyboard-bridge-media.yaml` 已于上述时间成功构建，应用 SHA-256 如上；`--hub --media` 最终链接检查通过，包含两个 HID 接口、HCD ABI 和复位包装器。此次**没有刷入独立镜像**。实现提交的 CI 同时运行带内存检查的测试及三种示例的构建／核验，状态以所链接的运行记录为准。


### Knob diagnostic trial / 旋钮诊断测试版

Integrated application `b26f8280f686e9ca977d3b289bd8838a42bed74edbc596fad544abe14e8d25d2` (2026-10-26 21:53:11 +08:00) contains the known-held recovery change and count-only timing diagnostics. OTA and successful boot were observed. At this recording the receiver was not on P4, so human regression and the idle-delay comparison are pending. The standalone media profile built and passed its linked audit, SHA-256 `78502721ca592d1d94fdcb006fcd9fdce60c1776331455fd31470f448f897573`; it was not flashed. Earlier hardware confirmations above apply to their named images, not automatically to this trial.

集成诊断应用 `b26f8280…`（上述时间）包含按实际按住状态恢复门控的改动及计数时序诊断。OTA 和成功启动确认已观察到；记录时接收器未连接 P4，人工回归与闲置延迟对照仍待完成。独立媒体配置已构建并通过链接检查，SHA-256 如上，未刷入。上方旧版本的硬件确认不能自动算作本测试版也已通过。


### Contact-maintenance follow-up / 旋钮触点处理后的跟进

The owner reported that first-detent misses persisted with the recovery fix, including with P4 output connected toward the PC. After keyboard support suggested reseating and cleaning the rotary contacts, the owner tried that and reported that it now seemed normal, choosing to observe. This supports a contact-related explanation; it is not a controlled separation of cleaning versus reseating, or a long-term reliability result. No further USB-format or pulse-timing changes were made based on speculation.

使用者反馈首格问题在门控修正后仍存在，P4 输出朝电脑连接时也会漏响应。随后客服建议重新插拔、清洁旋钮触点，使用者尝试后表示目前似乎恢复正常，先观察。这支持触点相关解释，但没有单独隔离擦拭与重插，也不是长期稳定性结论。没有据此继续试改 USB 格式或按键脉冲节奏。

The installed integrated diagnostic image is SHA-256 `5e030d5124b204afe3b563ff378d0886a1a797e5ab0d406dbeea9526470536ef` (2026-10-26 22:39:53 +08:00). It retains the known-held gate fix; the only follow-up code change extends media count diagnostics using a 2000-record per-boot quota, while keyboard count traces retain their five-minute limit. Build, linked descriptor audit, OTA and successful boot passed. A sample showed approximately 5–6ms input pulses and approximately 1ms forwarding/ACK delay, without media queue drops or gate suppression. Windows-side synchronized failure capture was not conclusive. External diagnostic collectors were stopped after the maintenance result; the owner is observing with this firmware retained. The previous implementation CI run passed all four test/build jobs.

当前保留的集成诊断镜像 SHA-256 如上（上述时间）。它保留已知按住状态的门控修正，后续代码只把媒体计数诊断延长为每次启动最多 2000 条；键盘计数仍在五分钟后停止。构建、描述符核验、OTA 和成功启动均通过。样本中输入脉冲约 5–6ms，转发／ACK 延迟约 1ms，没有媒体队列丢弃或门控拦截；Windows 侧的同步失败捕获没有得出确定结论。触点处理后已停止额外采集，按使用者要求保留当前固件观察。前一实现的 CI 四项测试／构建均通过。
