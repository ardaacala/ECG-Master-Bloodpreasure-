#include <Arduino.h>
#include "config.h"
#include "protocol.h"
#include "ui.h"
#include "fake_ecg.h"
#include "event_scheduler.h"
#include "wifi_master.h"
#include "ble_master.h"

static int64_t masterTimeUs() { return esp_timer_get_time(); }
static uint32_t eventId = 0;

struct AckPair {
    uint32_t eventId;
    bool wifiReceived;
    bool bleReceived;
    int64_t wifiAtUs;
    int64_t bleAtUs;
};
static AckPair ackPairs[16] = {};
static uint32_t lastStatusPublishMs = 0;
static uint32_t lastCrossLatencyPublishMs = 0;

static bool mixedSystemReady() {
    const WifiNodeStatus wifi = wifiNodeStatus(WIFI_NODE_ID);
    const BleNodeStatus ble = bleNodeStatus();
    return wifi.synced && ble.synced;
}

static bool sendTrigger(const EcgEvent &source) {
    if (!mixedSystemReady()) return false;
    const uint32_t leadUs = max(wifiRecommendedLeadUs(), bleRecommendedLeadUs());
    const uint32_t duration = source.type == PULSE_LARGE ? LARGE_PULSE_DURATION_US : SMALL_PULSE_DURATION_US;
    ScheduledEvent event{++eventId, source.type, source.targetMask,
                         source.atUs + leadUs, duration};
    TriggerPacket packet{MSG_TRIGGER, event.eventId, (uint8_t)source.type, source.targetMask,
                         event.executeAtMasterUs, duration};
    const bool wifiOk = wifiSendTrigger(packet);
    const bool bleOk = bleSendTrigger(packet);
    if (!wifiOk || !bleOk || !schedulerAdd(event)) return false;
    return true;
}

static void recordAck(uint32_t id, bool fromWifi, int64_t actualMasterUs) {
    AckPair &pair = ackPairs[id % 16];
    if (pair.eventId != id) pair = {id, false, false, 0, 0};
    if (fromWifi) { pair.wifiReceived = true; pair.wifiAtUs = actualMasterUs; }
    else { pair.bleReceived = true; pair.bleAtUs = actualMasterUs; }
    if (pair.wifiReceived && pair.bleReceived) {
        const uint32_t nowMs = millis();
        if (nowMs - lastCrossLatencyPublishMs >= UI_STATUS_INTERVAL_MS) {
            lastCrossLatencyPublishMs = nowMs;
            uiSetCrossTransportLatency(pair.bleAtUs - pair.wifiAtUs, true);
        }
#if ENABLE_VERBOSE_TIMING_LOGS
        Serial.printf("[MIXED ACK] #%lu BLE-WIFI:%+lld us\n",
                      (unsigned long)id,
                      (long long)(pair.bleAtUs - pair.wifiAtUs));
#endif
        pair.wifiReceived = pair.bleReceived = false;
    }
}

static void publishMixedStatus() {
    const uint32_t nowMs = millis();
    if (nowMs - lastStatusPublishMs < UI_STATUS_INTERVAL_MS) return;
    lastStatusPublishMs = nowMs;
    const WifiNodeStatus wifi = wifiNodeStatus(WIFI_NODE_ID);
    const BleNodeStatus ble = bleNodeStatus();
    uiSetNodeStatus(WIFI_NODE_ID, "WiFi", wifi.connected, wifi.synced, wifi.rttUs);
    uiSetNodeStatus(BLE_NODE_ID, "BLE", ble.connected, ble.synced, ble.rttUs);
    uiSetDeviceCounts((wifi.connected ? 1 : 0) + (ble.connected ? 1 : 0),
                      (wifi.synced ? 1 : 0) + (ble.synced ? 1 : 0));
    uiSetSystemReady(wifi.synced && ble.synced);
    WifiLedAck wifiAck{};
    while (wifiTakeLedAck(wifiAck)) recordAck(wifiAck.eventId, true, wifiAck.actualMasterUs);
    BleLedAck bleAck{};
    while (bleTakeLedAck(bleAck)) recordAck(bleAck.eventId, false, bleAck.actualMasterUs);
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("ECG WIFI + BLE MASTER start");
    if (!uiBegin()) Serial.println("[UI] INITIALIZATION FAILED");
    wifiMasterBegin();
    bleMasterBegin();
}

void loop() {
    const int64_t now = masterTimeUs();
    EcgEvent event;
    if (fakeEcgNextEvent(now, event)) {
        uiSetBpm(event.bpm);
        // UI veya ag islemi donguyu geciktirdiyse artik gecmiste kalan ECG
        // olayini slave'lere yollama. fakeEcg sonraki dongulerde hizla guncel
        // olaya yetisir; boylece gecmis trigger kuyrugu olusmaz.
        if (event.atUs + wifiRecommendedLeadUs() > now) {
            sendTrigger(event);
        }
    }
    schedulerTick(now);
    // Wi-Fi ve BLE Core 0'daki ayri ag gorevlerinde calisir. Core 1 yalnizca
    // zamanlama ve LVGL ekranini yonetir.
    publishMixedStatus();
    uiTick();
    delay(1);
}
