# Long-idle reliability investigation / 长时间闲置稳定性排查

## Report and evidence / 现象与证据

The tester returned after a long idle interval, found the keyboard unresponsive and then saw repeated input. When diagnostics were captured, the integrated P4 had run for about 18 hours, had roughly 479 kB free heap (historical minimum about 474 kB), no Ethernet disconnects and one queue overflow. USB input and output were then working. This does not show memory exhaustion, but a single snapshot cannot rule out every leak. The earlier incident was not captured in full, so its exact trigger remains unproven.

使用者反馈闲置很久后先无响应，随后重复输入。抓取诊断时，P4 已运行约 18 小时，空闲堆约 479 kB、历史最低约 474 kB，以太网断线为零，队列曾溢出一次；当时输入输出已恢复。没有观察到内存耗尽，但单次快照不能完全排除所有泄漏，也不能据此确定此前故障的唯一触发路径。

## Code changes / 代码修复

- Treat USB suspension separately from mere enumeration: `tud_mounted()` alone does not mean the host is currently polling.
- Detect lifecycle epochs, including a bus reset/re-enumeration too fast for the worker to observe an unmounted interval. Clear an abandoned in-flight flag at mount in the TinyUSB task.
- Timestamp input and output transitions. Discard queues older than 100 ms; never replay a burst of stale typing after recovery.
- Recover with all keys released and require a fresh physical release. A synthetic state reset cannot unlock the gate. Normally delivered held keys are not limited to 100 ms.
- If an active nonsuspended output transfer remains incomplete for over one second, attempt a real native PHY detach/reconnect without replaying the switch shortcut. Attempts are rate-limited to once per 30 seconds.
- Move optional LED control requests, cleanup and input restart into a separate serialized control task. A receiver control request can take several seconds to time out; it must not pause ordinary input/output forwarding. Handle ownership cleanup remains ordered after earlier LED requests.
- Request endpoint stop/start after an input transfer error. The pinned HID driver's error path does not automatically resubmit its interrupt transfer. Failures are counted/logged; hardware or driver faults may still require replugging.
- Add aggregate diagnostics for recoveries, expired transitions, in-flight age, worker delay, control errors and maximum control duration. Do not record typed reports or characters.

关键修复是区分“仍已枚举”和“主机正在读取”，在休眠／恢复／重新枚举时清空旧按键，并对积压设置 100 ms 上限。恢复先全松键，等待真实松键；已正常送达的长按不会被定时截断。LED 等控制操作独立执行，避免它们把按键转发一起阻塞。输入传输异常会请求重启读取，活动输出长时间无确认时尝试重新枚举。

## Validation limits / 验证边界

Portable tests cover FIFO wrap, release-only overflow, stale backlog discard, physical release gating, recovery boundaries and millisecond wrap, in addition to the existing HID/hotkey suite. Firmware descriptor audits ensure that this transport fix keeps the same simple output keyboard format.

The current update has not yet passed an overnight idle soak test. Artificial queue-age tests do not reproduce every wireless, electrical, operating-system sleep or KVM firmware condition. Recovering deliberately discards uncertain input; the first wake-up key may be consumed until all keys are released. Release and press again. Do not describe the earlier incident as conclusively fixed solely from a short reconnect test.

单元测试和固件编译不能代替整夜闲置、无线链路、电脑睡眠和 KVM 实机验证。当前恢复策略会主动丢弃不确定的旧输入，可能吃掉第一次唤醒按键；松开后重新按即可。短时间重连通过，也不能写成已完全解决原先长时间闲置后的问题。

The `firmware/` binary is still the initial **v0.1.0** standalone image and does not contain these source fixes. Build current source to use them. Original release artifacts remain available for comparison; check their manifest/source reference.

## Short hardware regression / 短时间实机回归

The updated core was tested in the integrated firmware: ordinary typing, intentional hold/release, bidirectional Ctrl+Alt+/ switching, output unplug/replug with input during disconnection, and a separate Windows Notepad Backspace test passed. Logs showed suspend -> attach, no output backlog, zero new overflows and no transfer/control errors. The source-only standalone configuration also compiled and passed its linked-descriptor audit; its newly built image was not flashed in this update. See [the test record](reliability-test-results.txt).

修复核心已在集成固件实测：普通输入、正常长按与松开、双向热键切换、输出线断开期间按键后重连，以及 Windows 记事本中的退格对照均正常。日志捕获到休眠后重新连接，没有旧输入积压、新溢出或传输错误。独立配置也已编译并通过描述符检查，但本次没有再次刷入独立镜像；不能把它写成新增独立固件实机验证。整夜闲置验证仍待完成。

Receiver-side hotplug was also tested twice: both interfaces returned and input/Backspace recovered each time. Physical removal produced low-level invalid-state URB/endpoint logs, followed by successful cleanup/re-enumeration; component error/overflow counters stayed zero. The final recovery count of seven is fully accounted for by startup and the deliberate output/receiver tests.

接收器端也进行了两次拔插：每次两个接口都重新建立，输入与退格恢复正常。物理拔下瞬间出现过底层 URB／端点 invalid-state 日志，之后清理和重新枚举成功，组件错误／溢出计数仍为零。最终七次恢复均对应启动及人为拔插测试，短测期间没有无法解释的自动恢复。

## Keyboard radio reconnection follow-up / 键盘无线重连跟进

The tester reproduced stuck Backspace by leaving the receiver on P4, powering the keyboard off for ten seconds, then on, and holding/releasing Backspace. Repeating the same keyboard power-cycle test with the receiver directly on the PC passed. During the P4 failure, the last keyboard report remained held, with callbacks=86, accepted=86, all 19-byte packets, no empty packets, parser rejects, overflow or reported transfer errors. The output queue was empty and completion delay was 0–1 ms. No later keyboard input callback carried the release. This excludes output backlog and a silently ignored empty packet for **that capture**, not every possible failure.

The opt-in `drain_auxiliary_interfaces` path keeps non-keyboard HID interfaces open and starts their input transfers. Auxiliary payloads are discarded inside the callback, without entering the keyboard queue or being logged/forwarded. A stable slot context and atomic keyboard flag route callbacks; auxiliary disconnects/errors are counted separately, with serialized cleanup/restart. Keyboard input remains descriptor-driven Report Protocol, and output remains one 8-byte Boot Keyboard interface. The component default is false; the shipped NuPhy-oriented example explicitly enables it.

Integrated image built **2026-10-19 21:41:52 +08:00**, SHA-256 `aefc389dec934c4c7327d1a03e7c5bf20350ff3f4c0feee0b5fc775020765f29`, passed three matched keyboard-off/on hold/release cycles, bidirectional Ctrl+Alt+/ switching and post-switch typing. Two keyboard plus two auxiliary interfaces stayed open; no overflow or new input/control/auxiliary errors were observed. The short capture counted **zero auxiliary reports**, so it does not prove a specific vendor payload queue was blocking keyboard release. Retaining interfaces also changes host-driver endpoint handling; the exact lower-layer mechanism is unproven. No switch back to the failing configuration was used to claim deterministic causation. Overnight idle stability remains unverified.

Optional `trace_key_counts: true` records interface-level held-key counts, packet lengths and input/desired/USB-completion timestamps for the first five minutes after boot. It uses a bounded nonblocking trace queue and prints in the main loop. Fast typing can drop diagnostic lines (`trace-drops`), independently of keyboard forwarding; a later busy-typing snapshot counted 328 trace drops while keyboard overflows remained zero. Do not treat the optional trace as a lossless event capture. It never logs key usages, typed characters or raw report payloads. DEBUG snapshots also expose callback/empty/8-byte/19-byte/other packet counters and maximum queue/transfer delay. These counters do not synthesize releases or change repeat behavior. No arbitrary long-hold cutoff is applied.

Forced Boot Protocol input stopped this receiver from typing and was reverted; recovery also required physically replugging the receiver. SET_IDLE was acknowledged but periodic input was not observed. GET_REPORT produced endpoint STALL and was not usable as authoritative current state. None of these unsuccessful experiments is in the published implementation.

中文：接收器留在 P4，只重启键盘本体可复现连删；相同条件直连电脑正常。故障时全部 86 次键盘回调都是 19 字节，没有空包、解析拒绝、队列溢出或后续松键回调，输出也没有积压。保留附加 HID 接口并轮询后，三轮相同重启／长按／松手测试及双向切换通过；收到的附加数据不会转发或写入日志。短测附加报告计数为零，仍不能证明某种厂商报告堵塞是唯一原因，也没有证明整夜待机稳定。强制 Boot 输入等失败实验已撤回。可选诊断只记数量、长度和时间，不记录按键码或输入文字。
