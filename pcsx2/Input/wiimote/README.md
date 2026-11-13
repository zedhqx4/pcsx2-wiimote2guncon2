# PCSX2 Wii Remote / GunCon 2 input

This integration exposes one Nintendo Wii Remote as a PCSX2 input source and feeds its IR camera position directly into the existing emulated USB GunCon 2 device.

## Setup

1. Pair the Wii Remote with the host OS and connect it before or while PCSX2 is running.
2. Make sure the host has a working sensor bar. The IR camera needs to see at least two IR points.
3. In PCSX2, enable the **Wiimote** input source.
4. Select **GunCon 2** for the desired USB port.
5. Enable **Use Wiimote IR Aiming** in that GunCon 2 device's settings.
6. Assign the Wii Remote buttons to the GunCon 2 controls in the normal USB binding page. The initial generic mappings make Wii Remote B the GunCon 2 trigger and Nunchuk Z the offscreen-shot input; the remaining buttons can be remapped.

When IR tracking is lost, GunCon 2 receives an offscreen position (0, 0). It does not fall back to the host mouse. When **Use Wiimote IR Aiming** is disabled, PCSX2 keeps its existing mouse/relative-input behavior.

With **Use Wiimote IR Aiming** enabled and no custom **Cursor Path**, PCSX2 displays a built-in crosshair. The crosshair disappears while the IR camera cannot see two usable points; this mirrors the offscreen report sent to the emulated gun.

The input log reports IR tracking acquisition/loss and, while unavailable, the number and coordinates of visible IR points. This helps distinguish a sensor-bar visibility problem from a report-decoding problem.

The HID worker reconnects after a read error or 1.5 seconds without any reports. Device discovery is retried automatically; the input state is cleared between connections.

There is not yet a dedicated Wii Remote calibration wizard. **Calibration Shot** is the GunCon 2 protocol's special calibration-report sequence, not a sensor-bar calibration routine. The existing manual screen configuration (center, scale, and screen dimensions) remains available for game-specific alignment after IR tracking is working.

## Implementation notes

- The Wiimote is discovered through SDL3 HIDAPI using Nintendo VID 0x057E and product IDs 0x0306 (RVL-CNT-01) or 0x0330 (RVL-CNT-01-TR).
- HID reads and writes run on a dedicated worker thread. Button transitions are queued so short presses are not discarded between PCSX2 input polls.
- Button and Nunchuk stick events are exposed through PCSX2's regular input-source and USB binding system.
- IR aiming is read by GunCon 2 directly from the Wiimote source. It does not call PCSX2's host-pointer update functions and does not synthesize mouse movement.
- The remote's single rumble motor receives the stronger of PCSX2's two motor channels when the Wiimote is assigned to a rumble-capable PAD. The emulated USB GunCon 2 protocol has no rumble output, so a GunCon-only configuration does not produce game-driven rumble.
- This first integration supports one connected Wii Remote. Additional remotes, other extension types, MotionPlus data and alternative Linux transports are not implemented here.
- Linux behavior depends on SDL3 HIDAPI being able to open the remote. A kernel driver that claims the HID interface or restrictive device permissions may prevent access; that needs validation on the target system.

The protocol and IR/Nunchuk math sources are vendored from the shared WIimoteDLL_Libusbk backend so the emulator integration can use the same protocol implementation.
