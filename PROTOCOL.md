# Version 2 Protocol Reference

Bu dosya master ile Wi-Fi/BLE slave'ler arasındaki wire sözleşmesidir. Struct
alanlarının sırası, tipi veya `packed` özelliği tek tarafta değiştirilmemelidir.

## Saat sözleşmesi

```text
clockOffsetUs = slaveClock - masterClock
localTargetUs = executeAtMasterUs + clockOffsetUs
masterActualUs = localActualUs - clockOffsetUs
```

Master her tetiklemeyi mevcut Version 2 kodunda ortak ve sabit 600 ms lead ile
geleceğe planlar. UI darbesi ve iki fiziksel slave aynı `executeAtMasterUs`
değerini hedefler. Global tekrar koruması 200 ms'dir.

## Mesaj tipleri

| Değer | Mesaj |
|---:|---|
| 1 | `MSG_SYNC_REQ` |
| 2 | `MSG_SYNC_RESP` |
| 3 | `MSG_SYNC_COMMIT` |
| 4 | `MSG_TRIGGER` |
| 5 | `MSG_LED_ON_ACK` |
| 6 | `MSG_HELLO` (Wi-Fi) |
| 7 | `MSG_DEVICE_STATUS` (Wi-Fi) |

## Packed boyutlar

| Paket | Bayt |
|---|---:|
| `SyncRequest` | 13 |
| `SyncResponse` | 21 |
| `SyncCommit` | 13 |
| Wi-Fi `TriggerPacket` | 26 |
| Wi-Fi `LedOnAck` | 17 |
| `HelloPacket` | 26 |
| `DeviceStatusPacket` | 11 |
| BLE `BleTriggerPacket` | 18 |
| BLE `BleLedOnAck` | 13 |

Wi-Fi paketi `targetMask` içerir. BLE slave'in eski ve daha küçük wire formatı
bu alanı içermez; master hedef filtresini `bleSendTrigger()` içinde yapar.

## Cihazlar

- Wi-Fi: mantıksal ID 1, `ECG_LEFT`, TCP 3333.
- BLE: mantıksal ID 2, reklam adı `LED_NODE_1`.

## BLE UUID'leri

```text
Slave service: 6e400001-b5a3-f393-e0a9-e50e24dcca9e
Slave RX:      6e400002-b5a3-f393-e0a9-e50e24dcca9e
Slave TX:      6e400003-b5a3-f393-e0a9-e50e24dcca9e

Phone service: 7d2e0001-8d7b-4f91-a8d3-4c7f2b6a1000
Phone RX:      7d2e0002-8d7b-4f91-a8d3-4c7f2b6a1000
Phone TX:      7d2e0003-8d7b-4f91-a8d3-4c7f2b6a1000
```

Telefon ayar komutu `BPM,SYS,DIA`, örneğin `72,130,60` biçimindedir.
