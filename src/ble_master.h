#pragma once
#include <stdint.h>
#include "protocol.h"

struct BleNodeStatus {
    bool connected;
    bool synced;
    uint32_t rttUs;
};
struct BleLedAck { uint32_t eventId; int64_t actualMasterUs; };

void bleMasterBegin();
void bleMasterTick();
BleNodeStatus bleNodeStatus();
bool bleSendTrigger(const TriggerPacket &packet);
bool bleTakeLedAck(BleLedAck &ack);
uint32_t bleRecommendedLeadUs();
