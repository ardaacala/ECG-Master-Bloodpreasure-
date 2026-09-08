#pragma once
#include <stdint.h>
#include <stddef.h>

constexpr size_t DEVICE_NAME_MAX_LENGTH = 24;

constexpr uint64_t maskForDevice(uint8_t deviceId) {
    if (deviceId < 1 || deviceId > 64) return 0;
    return 1ULL << (deviceId - 1);
}

enum MessageType : uint8_t { MSG_SYNC_REQ = 1, MSG_SYNC_RESP = 2, MSG_SYNC_COMMIT = 3, MSG_TRIGGER = 4, MSG_LED_ON_ACK = 5, MSG_HELLO = 6, MSG_DEVICE_STATUS = 7 };
enum PulseType : uint8_t { PULSE_SMALL = 1, PULSE_LARGE = 2 };

struct __attribute__((packed)) SyncRequest { uint8_t type; uint32_t requestId; int64_t t1; };
struct __attribute__((packed)) SyncResponse { uint8_t type; uint32_t requestId; int64_t t2; int64_t t3; };
struct __attribute__((packed)) SyncCommit { uint8_t type; uint32_t requestId; int64_t offsetUs; };
struct __attribute__((packed)) TriggerPacket { uint8_t type; uint32_t eventId; uint8_t pulseType; uint64_t targetMask; int64_t executeAtMasterUs; uint32_t ledOnTimeUs; };
// Slave'in planlanan ana gore gercek GPIO acma sapmasi. Saat baslangiclari
// farkli olsa bile master bu degerle slave'leri dogrudan karsilastirabilir.
struct __attribute__((packed)) LedOnAck { uint8_t type; uint32_t eventId; int32_t latenessUs; int64_t actualLocalUs; };
struct __attribute__((packed)) HelloPacket { uint8_t type; uint8_t deviceId; char deviceName[DEVICE_NAME_MAX_LENGTH]; };
struct __attribute__((packed)) DeviceStatusPacket { uint8_t type; uint8_t deviceId; int8_t rssiDbm; uint32_t lateDrops; uint32_t queueRejects; };

// Mevcut BLE Slave 2 protokolu targetMask ve lateness alanlarini icermiyor.
// Bunlar sadece BLE hattinda kullanilan uyumluluk paketleridir.
static const char SERVICE_UUID[] = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
static const char RX_UUID[] = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
static const char TX_UUID[] = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";
struct __attribute__((packed)) BleTriggerPacket { uint8_t type; uint32_t eventId; uint8_t pulseType; int64_t executeAtMasterUs; uint32_t ledOnTimeUs; };
struct __attribute__((packed)) BleLedOnAck { uint8_t type; uint32_t eventId; int64_t ledOnAtLocalUs; };

// iOS/Android BLE scanner applications connect to the master through this
// separate GATT service. Commands are UTF-8 text written to CONTROL_RX_UUID;
// replies are readable/notified from CONTROL_TX_UUID.
static const char CONTROL_SERVICE_UUID[] = "7d2e0001-8d7b-4f91-a8d3-4c7f2b6a1000";
static const char CONTROL_RX_UUID[]      = "7d2e0002-8d7b-4f91-a8d3-4c7f2b6a1000";
static const char CONTROL_TX_UUID[]      = "7d2e0003-8d7b-4f91-a8d3-4c7f2b6a1000";
