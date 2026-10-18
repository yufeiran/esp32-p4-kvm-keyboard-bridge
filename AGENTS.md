# Contributor notes

This repository is a standalone ESP32-P4 USB HID keyboard bridge. Read README.md (or README.zh-CN.md), docs/architecture.md and docs/compatibility.md before changing transport behavior.

- Keep the README order: what it does, how it works, then implementation/reproduction. Maintain both languages when behavior changes.
- Keep the default example offline and free of personal network, Home Assistant, KVM UART/IR and desktop-helper configuration.
- Do not claim that NKRO is the proven sole root cause, that USB 1.1 implies 6KRO, or that untested keyboard/KVM combinations work.
- Preserve the native PHY pull-up/pad detach and runtime routing fix unless replacement behavior has hardware evidence. Never burn eFuses as part of setup.
- Do not log typed key reports or characters. Static descriptors, connection metadata and aggregate counters are sufficient for diagnostics.
- Run the portable C++ tests for parser/hotkey changes and compile/audit the linked descriptors for USB/configuration changes. Hardware confirmations must name the actual tested image/configuration.
- Keep generated build files, personal configuration and credentials out of commits.
