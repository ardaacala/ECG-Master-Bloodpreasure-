#pragma once
#include <stdint.h>
#include "protocol.h"

void wifiMasterBegin();
void wifiMasterTick();
void wifiMasterPublishUi();
bool wifiSystemReady();
bool wifiSendTrigger(const TriggerPacket &packet);
bool wifiPublishTriggerState(const TriggerStatePacket &packet);
uint32_t wifiRecommendedLeadUs();

struct WifiNodeStatus {
    bool connected;
    bool synced;
    uint32_t rttUs;
    uint32_t jitterUs;
    int8_t rssiDbm;
};
struct WifiLedAck { uint32_t eventId; int64_t actualMasterUs; };
WifiNodeStatus wifiNodeStatus(uint8_t deviceId);
bool wifiTakeLedAck(WifiLedAck &ack);
