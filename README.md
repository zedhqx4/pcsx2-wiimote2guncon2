# PCSX2

![Windows Build Status](https://img.shields.io/github/actions/workflow/status/PCSX2/pcsx2/windows_build_matrix.yml?label=%F0%9F%96%A5%EF%B8%8F%20Windows%20Builds)
![Linux Build Status](https://img.shields.io/github/actions/workflow/status/PCSX2/pcsx2/linux_build_matrix.yml?label=%F0%9F%90%A7%20Linux%20Builds)
![MacOS Build Status](https://img.shields.io/github/actions/workflow/status/PCSX2/pcsx2/macos_build_matrix.yml?label=%F0%9F%8D%8E%20MacOS%20Builds)
[![Codacy Badge](https://app.codacy.com/project/badge/Grade/1f7c0d75fec74d6daa6adb084e5b4f71)](https://app.codacy.com/gh/PCSX2/pcsx2/dashboard?utm_source=github.com&utm_medium=referral&utm_content=PCSX2/pcsx2&utm_campaign=Badge_Grade)
[![Discord Server](https://img.shields.io/discord/309643527816609793?color=%235CA8FA&label=PCSX2%20Discord&logo=discord&logoColor=white)](https://discord.com/invite/TCz3t9k)

PCSX2 is a free and open-source PlayStation 2 (PS2) emulator. Its purpose is to emulate the PS2's hardware, using a combination of MIPS CPU [Interpreters](<https://en.wikipedia.org/wiki/Interpreter_(computing)>), [Recompilers](https://en.wikipedia.org/wiki/Dynamic_recompilation) and a [Virtual Machine](https://en.wikipedia.org/wiki/Virtual_machine) which manages hardware states and PS2 system memory. This allows you to play PS2 games on your PC, with many additional features and benefits.

## Project Details

PCSX2 has been in development for more than 20 years. Past versions could only run a few public domain game demos, but newer versions can run most games at full speed, including popular titles such as Final Fantasy X and Devil May Cry 3. Visit the [PCSX2 compatibility list](https://pcsx2.net/compat/) to check the latest compatibility status of games (with more than 2500 titles tested).

Installers and binaries for both stable and nightly builds are available from [our website](https://pcsx2.net/downloads/).

## System Requirements

PCSX2 supports Windows, Linux, and Mac platforms. Our [setup documentation page](https://pcsx2.net/docs/setup/requirements) contains additional details on software and hardware requirements.

Please note that a BIOS dump from a legitimately-owned PS2 console is required to use the emulator. For more information, visit [this page](https://pcsx2.net/docs/setup/bios/).

## Contributing / Building

PCSX2 supports translation into other languages using [Crowdin](https://crowdin.com/project/pcsx2-emulator).

See the [Contribution Guide](https://pcsx2.net/docs/contributing/) for more info on how to contribute.

## About This Fork — One Wiimote, One GunCon 2, World Domination

This is an independent, experimental PCSX2 fork. The first major objective is to make a real Nintendo Wii Remote work as a light gun through PCSX2's **native USB GunCon 2 emulation**.

The idea is simple: use the Wii Remote's IR camera as the pointing device, then feed its calculated position directly into the emulated GunCon 2. This is **not mouse emulation**: the integration does not move the host pointer or synthesize mouse input. It uses PCSX2's existing GunCon 2 device path, so games see a light gun rather than a mouse.

### What works today

- **Wii Remote input source:** the remote is discovered through SDL3 HIDAPI and appears in PCSX2's normal input configuration.
- **Native GunCon 2 aiming:** IR coordinates are sent directly to the emulated USB GunCon 2.
- **On-screen crosshair:** a built-in reticle is shown while valid IR tracking is available; no custom cursor image is required.
- **Buttons and Nunchuk:** Wii Remote buttons and the Nunchuk stick/buttons are exposed through PCSX2's regular binding system.
- **Reconnect handling:** the input worker detects failed or stalled HID reports, clears stale input, and retries device discovery.
- **IR diagnostics:** the log reports tracking acquisition/loss and visible IR points to help troubleshoot sensor-bar visibility and tracking.

A sensor bar (or another source of at least two visible IR points) is required for aiming. If tracking is lost, the emulated gun is sent an offscreen position. A dedicated multi-point calibration wizard is not implemented yet; the existing GunCon 2 screen-position settings can be used for manual alignment.

### Current scope and limitations

This is an early implementation, not a finished general-purpose light-gun platform.

- The current integration supports one Wii Remote at a time.
- Nunchuk input is supported; other extensions and MotionPlus data are outside the current scope.
- Windows is the currently tested platform. Linux access through SDL3 HIDAPI and macOS behavior still need validation on real systems.
- The emulated USB GunCon 2 protocol has no game-driven rumble output. The Wii Remote's motor can receive rumble from PCSX2 when it is also assigned to a rumble-capable PAD; a GunCon-only setup will not vibrate from in-game GunCon commands.

### The bigger plan

The long-term goal is larger than one emulator or one operating system:

1. **Build on a reusable Wiimote backend.** Keep protocol handling and IR/Nunchuk math independent from emulator-specific code, sharing work with the [WIimoteDLL_Libusbk](https://github.com/zedhqx4/WIimoteDLL_Libusbk) project.
2. **Make the transport genuinely cross-platform.** Validate and improve support for Windows, Linux, and macOS rather than treating one operating system as the only target.
3. **Bring native light-gun support to more emulators.** Explore integrations with other emulators through their own light-gun APIs (for example, GunCon/Justifier or generic light-gun input), instead of falling back to host mouse simulation.
4. **Keep the implementation testable and maintainable.** Prefer small, verifiable changes, clear diagnostics, and automated tests for protocol and aiming behavior.

The ambition is world domination. The implementation strategy is incremental engineering. 😼

This fork is experimental and is not an official PCSX2 release. Features, platform support, and behavior may change as development and hardware testing continue.
