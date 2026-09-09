#pragma once

#include <stdint.h>

enum MessageType : uint8_t {
    MSG_SYNC_REQ = 1,
    MSG_SYNC_RESP = 2,
    MSG_SYNC_COMMIT = 3,
    MSG_TRIGGER = 4,
    MSG_LED_ON_ACK = 5,
    MSG_TRIGGER_STATE = 8,
    MSG_TRIGGER_STATE_ACK = 9
};

enum PulseType : uint8_t {
    PULSE_SMALL = 1,
    PULSE_LARGE = 2
};

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

static const char SERVICE_UUID[] = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
static const char RX_UUID[] = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
static const char TX_UUID[] = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

struct __attribute__((packed)) SyncRequest {
    uint8_t type;
    uint32_t requestId;
    int64_t t1;
};

struct __attribute__((packed)) SyncResponse {
    uint8_t type;
    uint32_t requestId;
    int64_t t2;
    int64_t t3;
};

struct __attribute__((packed)) SyncCommit {
    uint8_t type;
    uint32_t requestId;
    int64_t offsetUs;
};

struct __attribute__((packed)) TriggerPacket {
    uint8_t type;
    uint32_t eventId;
    uint8_t pulseType;
    int64_t executeAtMasterUs;
    uint32_t ledOnTimeUs;
};

struct __attribute__((packed)) LedOnAck {
    uint8_t type;
    uint32_t eventId;
    int64_t ledOnAtLocalUs;
};

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

