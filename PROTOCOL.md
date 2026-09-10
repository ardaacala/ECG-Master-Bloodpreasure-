# Version 3 Protocol Reference

Bu dosya master ile Wi-Fi/BLE slave'ler arasındaki wire sözleşmesidir. Struct
alanlarının sırası, tipi veya `packed` özelliği tek tarafta değiştirilmemelidir.

## Saat sözleşmesi

```text
clockOffsetUs = slaveClock - masterClock
localTargetUs = executeAtMasterUs + clockOffsetUs
masterActualUs = localActualUs - clockOffsetUs
```

V3 state-trigger paketi ve ACK davranisinin ayrintili entegrasyon sozlesmesi
[docs/STATE_TRIGGER_PROTOCOL_V3.md](docs/STATE_TRIGGER_PROTOCOL_V3.md) dosyasindadir.
Master ilk fazi ortak ve sabit 600 ms lead ile gelecege planlar. Slave'ler bu
fazdan sonra BPM ritmini yerel uretir. UI tekrar korumasi 200 ms'dir.

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
| 8 | `MSG_TRIGGER_STATE` |
| 9 | `MSG_TRIGGER_STATE_ACK` |

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
| `TriggerStatePacket` | 21 |
| `TriggerStateAck` | 9 |

Wi-Fi paketi `targetMask` içerir. BLE slave'in eski ve daha küçük wire formatı
bu alanı içermez; master hedef filtresini `bleSendTrigger()` içinde yapar.

## Cihazlar

- Wi-Fi: mantıksal ID 1, `ECG_LEFT`, TCP 3333.
- BLE: mantıksal ID 2, reklam adı `RTCOUND20180`.

## BLE UUID'leri

```text
Slave service: 6e400001-b5a3-f393-e0a9-e50e24dcca9e
Slave RX:      6e400002-b5a3-f393-e0a9-e50e24dcca9e
Slave TX:      6e400003-b5a3-f393-e0a9-e50e24dcca9e

Phone service: 7d2e0001-8d7b-4f91-a8d3-4c7f2b6a1000
Phone RX:      7d2e0002-8d7b-4f91-a8d3-4c7f2b6a1000
Phone TX:      7d2e0003-8d7b-4f91-a8d3-4c7f2b6a1000

PC OTA service: 7d2e0011-8d7b-4f91-a8d3-4c7f2b6a1000
PC OTA control: 7d2e0012-8d7b-4f91-a8d3-4c7f2b6a1000
PC OTA data:    7d2e0013-8d7b-4f91-a8d3-4c7f2b6a1000
PC OTA status:  7d2e0014-8d7b-4f91-a8d3-4c7f2b6a1000
```

Telefon ayar komutu `BPM,SYS,DIA`, örneğin `72,130,60` biçimindedir.

## PC BLE OTA paketi

Tüm çok baytlı alanlar little-endian'dır. Control `START` paketi sırasıyla
`uint8 command=1`, `uint32 imageSize`, `uint32 imageCrc32`,
`uint8 passwordLength` ve parola baytlarını taşır. Data paketinde `uint32 offset`
ve en fazla 240 firmware baytı bulunur. Her başarılı flash yazımından sonra status
karakteristiği `ACK` ve beklenen sonraki offset'i bildirir. `FINISH` (`command=2`)
sonrasında boyut, CRC32 ve ESP OTA imajı doğrulanır; ancak bundan sonra boot
partition değiştirilir. `ABORT` komutu `3`'tür.

Status yapısı 9 bayttır: `uint8 code`, `uint32 nextOffset`, `int32 detail`.
Kodlar READY=1, ACK=2, COMPLETE=3, ABORTED=4 ve ERROR=0x80'dir.
