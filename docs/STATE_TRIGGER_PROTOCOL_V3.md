# State Trigger Protocol V3

This document is the integration contract for `v3.0.0`, named
**State based Trigger BLE Master Software**.

## Purpose

V2 sent one transport packet for every synthetic heartbeat. V3 sends retained
intent instead: a slave receives the latest BPM and pressure-zone state, stores
it, and generates its own output locally. The unchanged state is repeated with
a low-rate exponential heartbeat rather than once per beat. This permits a slave to select an LED,
sound, video, haptic pattern, or another response without changing the master's
fixed sensor input/output behavior.

Clock synchronization is periodic and independent from state delivery.

## Wire packets

All fields are little-endian and structs are packed. The authoritative C++
definitions are in `include/protocol.h` and are copied into both slave projects.

`TriggerStatePacket` is 21 bytes:

| Offset | Type | Field | Meaning |
|---:|---|---|---|
| 0 | `uint8` | `type` | `MSG_TRIGGER_STATE` (`8`) |
| 1 | `uint8` | `protocolVersion` | `3` |
| 2 | `uint16` | `receiverDeviceId` | `0` broadcast, `1` Wi-Fi, `2` BLE |
| 4 | `uint32` | `stateRevision` | Master revision used for ACK matching |
| 8 | `uint16` | `bpm` | 30 through 240 |
| 10 | `uint8` | `pressureZone` | 0 below, 1 inside, 2 above |
| 11 | `uint16` | `actionDelayMs` | Additional delay, 0 through 60000 ms |
| 13 | `int64` | `effectiveAtMasterUs` | Common master-clock phase anchor |

`TriggerStateAck` is 9 bytes:

| Offset | Type | Field | Meaning |
|---:|---|---|---|
| 0 | `uint8` | `type` | `MSG_TRIGGER_STATE_ACK` (`9`) |
| 1 | `uint8` | `protocolVersion` | `3` |
| 2 | `uint16` | `deviceId` | Slave ID |
| 4 | `uint32` | `stateRevision` | Exact received revision |
| 8 | `uint8` | `result` | 1 applied, 2 unchanged, 3 invalid, 4 persist failed |

## Master rules

1. Derive pressure zone from the cleaned sensor value:
   `P <= DIA` is below, `DIA < P < SYS` is inside, and `P >= SYS` or ADC
   saturation is above.
2. Publish a new revision only when BPM, zone, or action delay changes.
3. Send immediately for a new revision and after a matching slave reconnects.
4. While state is unchanged, send the same packet after 1, 2, 4, 8, 10, 10...
   seconds. Ten seconds is the maximum interval.
5. Reset this exponential schedule on a state change or reconnect.
6. Treat `APPLIED` and `UNCHANGED` ACKs as health confirmation. An ACK does not
   stop the low-rate heartbeat.
7. Continue periodic clock sync separately.

The current topology creates one packet for device 1 over Wi-Fi and one packet
for device 2 over BLE. `actionDelayMs` is currently zero. The first effective
time is scheduled 600 ms ahead so the master UI and slave output share a phase.

## Slave rules

1. Validate version, receiver ID, BPM, zone, and delay before applying a packet.
2. Apply every valid packet immediately so heartbeat/reconnect delivery can
   re-anchor output phase, even when its semantic content is unchanged.
3. Persist only BPM, zone, and delay. Do not persist the absolute timestamp.
4. Compare semantic content before writing NVS. A heartbeat duplicate must return
   `UNCHANGED` without another flash write.
5. Return `PERSIST_FAILED` if new content could not be saved; do not falsely ACK
   it as durable.
6. On reboot, restore the retained state. With zone `INSIDE`, generate a local
   beat every `60,000,000 / BPM` microseconds. With `BELOW` or `ABOVE`, stop the
   output immediately.

The bundled LED slaves use a 100 ms active pulse. A more capable slave should
replace that output action with its media/state handler while preserving packet
validation, NVS de-duplication, ACK, timing, and receiver-ID behavior.

## Compatibility

The V2 packet definitions remain in the headers to make staged migrations and
diagnostics possible, but the V3 master does not transmit them automatically.
Phone manual-trigger commands create a local monitor pulse only. BPM or
`BPM,SYS,DIA` commands update the state publisher and therefore propagate when
their semantic result changes.
