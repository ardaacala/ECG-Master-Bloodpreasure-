# Protocol Reference

Bu belge `ecgmaster_wifi_ble` son kaynak ağacındaki wire protocol sözleşmesini özetler.

## Ortak mesaj tipleri

| Value | Message |
|---:|---|
| 1 | `MSG_SYNC_REQ` |
| 2 | `MSG_SYNC_RESP` |
| 3 | `MSG_SYNC_COMMIT` |
| 4 | `MSG_TRIGGER` |
| 5 | `MSG_LED_ON_ACK` |
| 6 | `MSG_HELLO` — Wi-Fi |
| 7 | `MSG_DEVICE_STATUS` — Wi-Fi |

## Clock convention

```text
clockOffsetUs = slaveClock - masterClock
localTargetUs = executeAtMasterUs + clockOffsetUs
masterActualUs = localActualUs - clockOffsetUs
```

## Packed sizes

| Packet | Bytes |
|---|---:|
| `SyncRequest` | 13 |
| `SyncResponse` | 21 |
| `SyncCommit` | 13 |
| Wi-Fi `TriggerPacket` | 26 |
| Wi-Fi `LedOnAck` | 17 |
| `HelloPacket` | 26 |
| `DeviceStatusPacket` | 11 |
| BLE `TriggerPacket` | 18 |
| BLE `LedOnAck` | 13 |

## Wi-Fi TriggerPacket

```cpp
struct __attribute__((packed)) TriggerPacket {
    uint8_t type;
    uint32_t eventId;
    uint8_t pulseType;
    uint64_t targetMask;
    int64_t executeAtMasterUs;
    uint32_t ledOnTimeUs;
};
```

## BLE TriggerPacket

```cpp
struct __attribute__((packed)) TriggerPacket {
    uint8_t type;
    uint32_t eventId;
    uint8_t pulseType;
    int64_t executeAtMasterUs;
    uint32_t ledOnTimeUs;
};
```

BLE Slave 2 wire packet'inde `targetMask` yoktur. Master hedef filtresini BLE transport sınırında yapar ve yalnızca hedef uygunsa BLE compatibility packet'i kuyruğa koyar.

## ACK

Wi-Fi ACK:

```cpp
struct __attribute__((packed)) LedOnAck {
    uint8_t type;
    uint32_t eventId;
    int32_t latenessUs;
    int64_t actualLocalUs;
};
```

BLE ACK:

```cpp
struct __attribute__((packed)) LedOnAck {
    uint8_t type;
    uint32_t eventId;
    int64_t ledOnAtLocalUs;
};
```

Her iki ACK de fiziksel çıkışın çalıştığı yerel zamanı master zaman alanına çevirmek için kullanılır.

## BLE UUID

```text
Service: 6e400001-b5a3-f393-e0a9-e50e24dcca9e
RX:      6e400002-b5a3-f393-e0a9-e50e24dcca9e
TX:      6e400003-b5a3-f393-e0a9-e50e24dcca9e
```

Wire struct alan sırası/tipi/boyutu değiştirilirse master ve etkilenen slave birlikte güncellenmeli ve yeniden derlenmelidir.
