# Repository instructions for coding agents

This is hardware firmware, not a generic ESP32 sample. Preserve the verified
Version 2 behavior unless the user explicitly requests a hardware migration.

## Required workflow

1. Read `README.md`, `platformio.ini`, `include/config.h`, and the relevant source.
2. Keep the exact PlatformIO platform and library versions pinned.
3. Build with `python -m platformio run` after source or configuration changes.
4. Never guess the upload port. Confirm the ESP32-S3 USB VID:PID (`303A:1001`).
5. Never upload this master firmware to CP210x Wi-Fi/BLE slave ports.
6. Report changed files, build result, and whether hardware upload actually ran.

## Hardware invariants

- Target: Waveshare ESP32-S3 Touch AMOLED 1.43, 466x466, 16 MB flash/8 MB PSRAM.
- Pressure ADC is GPIO2. Onboard battery ADC is GPIO4 with divider ratio 3.0.
- Display QSPI pins are 9–14 and reset is 21.
- Touch/QMI8658 shared I2C is SDA47/SCL48. Do not reassign or initialize the IMU
  as part of unrelated changes.
- The pressure sensor expects nominal 0.2–2.7 V for 0–40 kPa and startup auto-zero.
- `slave1_wifi` is logical device 1, GPIO2 active-high in its default environment.
- `slave2_ble` is logical device 2, advertises `LED_NODE_1`, GPIO4 active-low.

## Timing and concurrency invariants

- UI is the priority workload: UI task stays pinned to Core 1 at 30 Hz.
- Sensor, Wi-Fi, BLE, and Arduino loop work stay non-blocking on Core 0.
- Do not add flash/NVS writes, network calls, scanning, or long serial output to
  the UI task.
- Do not add busy loops. FreeRTOS tasks must block/yield.
- `sendPulseTrigger()` is the single global de-duplication point.
- Physical slaves and UI must use the same scheduled execution timestamp.
- Synthetic pulse generation is the only automatic beat source. Do not re-enable
  the disabled real cuff-pulse detector unless explicitly requested.
- Preserve 30–240 BPM, 200 ms duplicate guard, 600 ms fixed trigger lead, and
  pressure-range behavior unless a requested change specifically targets them.

## BLE contract

- Phone advertisement: `BP_TRIGGER_MASTER`.
- Primary settings command: ASCII `BPM,SYS,DIA`, e.g. `72,130,60`.
- A BPM-only integer keeps the current pressure range.
- UUIDs and packed transport structs in `include/protocol.h` are shared contracts;
  changing them requires matching slave/app changes.
- When changing a transport packet, update and build its matching slave project.

## Repository hygiene

- Do not commit `.pio`, COM-port overrides, debug logs, local drivers, or `.codex_*`.
- Do not commit either master or slave `secrets.h` file. Public builds use the
  `CHANGE_ME` fallback; local hardware builds use ignored secrets files.
- Prefer small changes; do not rewrite the working architecture for style alone.
