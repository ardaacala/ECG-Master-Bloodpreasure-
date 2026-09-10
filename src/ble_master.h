#pragma once
#include <stdint.h>
#include "protocol.h"

struct BleNodeStatus {
    bool connected;
    bool synced;
    uint32_t rttUs;
};
struct BleLedAck { uint32_t eventId; int64_t actualMasterUs; };
enum BlePhoneCommandType : uint8_t {
    BLE_PHONE_SET_BPM,
    BLE_PHONE_TRIGGER
};
struct BlePhoneCommand {
    BlePhoneCommandType type;
    uint16_t bpm;
    uint16_t systolicMmHg;
    uint16_t diastolicMmHg;
    bool hasPressureRange;
    PulseType pulseType;
    uint64_t targetMask; // 0 = tum bagli slave'ler
};

void bleMasterBegin();
void bleMasterTick();
BleNodeStatus bleNodeStatus();
bool bleSendTrigger(const TriggerPacket &packet);
bool blePublishTriggerState(const TriggerStatePacket &packet);
bool bleTakeLedAck(BleLedAck &ack);
bool bleTakePhoneCommand(BlePhoneCommand &command);
void blePublishBpm(uint16_t bpm, uint16_t systolicMmHg,
                   uint16_t diastolicMmHg);
void blePublishBatteryLevel(uint8_t percent, bool valid);
void blePublishPhoneResult(uint32_t eventId, bool wifiQueued, bool bleQueued);
uint32_t bleRecommendedLeadUs();
