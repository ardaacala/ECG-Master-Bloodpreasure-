# Repository instructions for coding agents

This is hardware firmware, not a generic ESP32 sample. Preserve the verified
Version 3 behavior unless the user explicitly requests a hardware migration.

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
- ETA6098 `STAT` is not routed to an ESP32 GPIO on this board. Do not claim exact
  charger detection from software. Preserve the charge estimator's 82 kOhm
  ISET/500 mA, 1500 mAh CC/CV model and fast USB-removal/OCV correction.
- `slave1_wifi` is logical device 1, GPIO2 active-high in its default environment.
- `slave2_ble` is logical device 2, advertises `LED_NODE_1`, GPIO4 active-low.

## Timing and concurrency invariants

- UI is the priority workload: UI task stays pinned to Core 1 at 30 Hz.
- Sensor, Wi-Fi, BLE, and Arduino loop work stay non-blocking on Core 0.
- Do not add flash/NVS writes, network calls, scanning, or long serial output to
  the UI task.
- Do not add busy loops. FreeRTOS tasks must block/yield.
- `sendPulseTrigger()` schedules local UI pulses only; it must not send a packet
  on every beat.
- V3 physical slaves retain the latest trigger state and generate BPM locally.
  Their first beat and the UI use the same scheduled execution timestamp.
- Send `TriggerStatePacket` only when BPM/zone/delay changes or once after
  reconnect. Retry at most three times at 500 ms until a matching revision ACK.
- Periodic time synchronization remains separate from trigger-state delivery.
- Synthetic pulse generation is the only automatic beat source. Do not re-enable
  the disabled real cuff-pulse detector unless explicitly requested.
- Preserve 30–240 BPM, 200 ms local duplicate guard, 600 ms fixed trigger lead, and
  pressure-range behavior unless a requested change specifically targets them.

## BLE contract

- Phone advertisement: `BP_TRIGGER_MASTER`.
- Primary settings command: ASCII `BPM,SYS,DIA`, e.g. `72,130,60`.
- A BPM-only integer keeps the current pressure range.
- UUIDs and packed transport structs in `include/protocol.h` are shared contracts;
  changing them requires matching slave/app changes.
- V3 state routing uses receiver ID 0 for broadcast, 1 for the Wi-Fi slave and
  2 for the BLE slave. Slaves ACK the exact revision and must not rewrite flash
  for an unchanged semantic state.
- When changing a transport packet, update and build its matching slave project.
- PC BLE OTA uses the dedicated `BLE_OTA_*` UUIDs and packed structs in
  `include/protocol.h`; keep `tools/ble_ota_upload.py` in sync with them.
- BLE OTA accepts only the environment's `firmware.bin`, never the combined
  `firmware.factory.bin`. Preserve offset ACK, CRC32 and ESP image validation.

## Repository hygiene

- Do not commit `.pio`, COM-port overrides, debug logs, local drivers, or `.codex_*`.
- Do not commit either master or slave `secrets.h` file. The documented project
  default is `ECG_MASTER` / `ecgmaster123` in both committed config fallbacks;
  local overrides use ignored secrets files and must remain identical.
- Prefer small changes; do not rewrite the working architecture for style alone.
