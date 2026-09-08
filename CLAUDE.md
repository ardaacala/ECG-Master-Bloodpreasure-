# Claude Code guidance

Follow [AGENTS.md](AGENTS.md) as the authoritative repository instructions.

Before editing, identify whether the change affects the 30 Hz UI timeline, the
shared trigger timestamp, the BLE/Wi-Fi packet contract, or fixed board pins.
Preserve those invariants and run `python -m platformio run` before declaring a
change complete. Do not upload until the ESP32-S3 port is positively identified;
CP210x ports belong to slave devices.
