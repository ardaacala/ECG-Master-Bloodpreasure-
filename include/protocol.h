#pragma once
#include <stdint.h>
#include <stddef.h>

constexpr size_t DEVICE_NAME_MAX_LENGTH = 24;

constexpr uint64_t maskForDevice(uint8_t deviceId) {
    if (deviceId < 1 || deviceId > 64) return 0;
    return 1ULL << (deviceId - 1);
}

enum MessageType : uint8_t {
    MSG_SYNC_REQ = 1,
    MSG_SYNC_RESP = 2,
    MSG_SYNC_COMMIT = 3,
    MSG_TRIGGER = 4,          // Legacy V2 one-shot pulse.
    MSG_LED_ON_ACK = 5,       // Legacy V2 one-shot acknowledgement.
    MSG_HELLO = 6,
    MSG_DEVICE_STATUS = 7,
    MSG_TRIGGER_STATE = 8,    // V3 retained output state.
    MSG_TRIGGER_STATE_ACK = 9
};
enum PulseType : uint8_t { PULSE_SMALL = 1, PULSE_LARGE = 2 };
enum PressureZone : uint8_t {
    PRESSURE_BELOW = 0,
    PRESSURE_INSIDE = 1,
    PRESSURE_ABOVE = 2
};
enum TriggerStateApplyResult : uint8_t {
    TRIGGER_STATE_APPLIED = 1,
    TRIGGER_STATE_UNCHANGED = 2,
    TRIGGER_STATE_INVALID = 3,
    TRIGGER_STATE_PERSIST_FAILED = 4
};

struct __attribute__((packed)) SyncRequest { uint8_t type; uint32_t requestId; int64_t t1; };
struct __attribute__((packed)) SyncResponse { uint8_t type; uint32_t requestId; int64_t t2; int64_t t3; };
struct __attribute__((packed)) SyncCommit { uint8_t type; uint32_t requestId; int64_t offsetUs; };
struct __attribute__((packed)) TriggerPacket { uint8_t type; uint32_t eventId; uint8_t pulseType; uint64_t targetMask; int64_t executeAtMasterUs; uint32_t ledOnTimeUs; };
// Slave'in planlanan ana gore gercek GPIO acma sapmasi. Saat baslangiclari
// farkli olsa bile master bu degerle slave'leri dogrudan karsilastirabilir.
struct __attribute__((packed)) LedOnAck { uint8_t type; uint32_t eventId; int32_t latenessUs; int64_t actualLocalUs; };
struct __attribute__((packed)) HelloPacket { uint8_t type; uint8_t deviceId; char deviceName[DEVICE_NAME_MAX_LENGTH]; };
struct __attribute__((packed)) DeviceStatusPacket { uint8_t type; uint8_t deviceId; int8_t rssiDbm; uint32_t lateDrops; uint32_t queueRejects; };

// State-based V3 contract. The master sends immediately on semantic changes or
// reconnect, then repeats unchanged state with a 1..10 second exponential
// heartbeat. Periodic clock synchronization remains separate.
// receiverDeviceId=0 means broadcast.
struct __attribute__((packed)) TriggerStatePacket {
    uint8_t type;
    uint8_t protocolVersion;
    uint16_t receiverDeviceId;
    uint32_t stateRevision;
    uint16_t bpm;
    uint8_t pressureZone;
    uint16_t actionDelayMs;
    int64_t effectiveAtMasterUs;
};

struct __attribute__((packed)) TriggerStateAck {
    uint8_t type;
    uint8_t protocolVersion;
    uint16_t deviceId;
    uint32_t stateRevision;
    uint8_t result;
};

static_assert(sizeof(TriggerStatePacket) == 21,
              "TriggerStatePacket wire layout changed");
static_assert(sizeof(TriggerStateAck) == 9,
              "TriggerStateAck wire layout changed");

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

// PC-to-master BLE firmware update service. This is intentionally separate
// from the phone-control and physical-slave services so their contracts remain
// unchanged. All multi-byte fields are little-endian.
static const char BLE_OTA_SERVICE_UUID[] = "7d2e0011-8d7b-4f91-a8d3-4c7f2b6a1000";
static const char BLE_OTA_CONTROL_UUID[] = "7d2e0012-8d7b-4f91-a8d3-4c7f2b6a1000";
static const char BLE_OTA_DATA_UUID[]    = "7d2e0013-8d7b-4f91-a8d3-4c7f2b6a1000";
static const char BLE_OTA_STATUS_UUID[]  = "7d2e0014-8d7b-4f91-a8d3-4c7f2b6a1000";

enum BleOtaCommand : uint8_t {
    BLE_OTA_CMD_START = 1,
    BLE_OTA_CMD_FINISH = 2,
    BLE_OTA_CMD_ABORT = 3
};

enum BleOtaStatusCode : uint8_t {
    BLE_OTA_STATUS_READY = 1,
    BLE_OTA_STATUS_ACK = 2,
    BLE_OTA_STATUS_COMPLETE = 3,
    BLE_OTA_STATUS_ABORTED = 4,
    BLE_OTA_STATUS_ERROR = 0x80
};

struct __attribute__((packed)) BleOtaStartHeader {
    uint8_t command;
    uint32_t imageSize;
    uint32_t imageCrc32;
    uint8_t passwordLength;
};

struct __attribute__((packed)) BleOtaStatus {
    uint8_t code;
    uint32_t nextOffset;
    int32_t detail;
};
