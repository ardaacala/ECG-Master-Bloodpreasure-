#include "ble_master.h"
#include "config.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

static NimBLEClient *client = nullptr;
static NimBLERemoteCharacteristic *rx = nullptr;
static NimBLERemoteCharacteristic *tx = nullptr;
static const NimBLEAdvertisedDevice *pendingDevice = nullptr;
static bool connected = false;
static bool synced = false;
static int64_t offsetUs = 0;
static int64_t bestRttUs = INT64_MAX;
static int64_t lastRttUs = 0;
static int64_t pendingT1 = 0;
static uint32_t pendingRequest = 0;
static uint32_t nextRequest = 0;
static uint32_t lastSyncRequestMs = 0;
static uint32_t lastSyncMs = 0;
static QueueHandle_t ackQueue = nullptr;
static QueueHandle_t triggerQueue = nullptr;
static TaskHandle_t bleTaskHandle = nullptr;
static volatile bool publishedConnected = false;
static volatile bool publishedSynced = false;
static volatile uint32_t publishedRttUs = 0;

static constexpr uint8_t BLE_TRIGGER_QUEUE_LENGTH = 16;

class ClientCallbacks : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient *, int reason) override {
        connected = false; synced = false; rx = nullptr; tx = nullptr;
        bestRttUs = INT64_MAX;
        Serial.printf("[BLE] %s disconnected, reason=%d\n", BLE_NODE_NAME, reason);
    }
};
static ClientCallbacks clientCallbacks;

class ScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice *device) override {
        if (connected || pendingDevice || !device->haveName() ||
            device->getName() != BLE_NODE_NAME) return;
        pendingDevice = device;
        NimBLEDevice::getScan()->stop();
    }
};
static ScanCallbacks scanCallbacks;

static void onNotify(NimBLERemoteCharacteristic *characteristic,
                     uint8_t *data, size_t length, bool) {
    if (characteristic != tx || !data || length < 1) return;
    if (data[0] == MSG_LED_ON_ACK && length == sizeof(BleLedOnAck)) {
        BleLedOnAck source{};
        memcpy(&source, data, sizeof(source));
        BleLedAck ack{source.eventId, source.ledOnAtLocalUs - offsetUs};
        if (ackQueue) xQueueSend(ackQueue, &ack, 0);
        return;
    }
    if (data[0] != MSG_SYNC_RESP || length != sizeof(SyncResponse)) return;
    SyncResponse response{};
    memcpy(&response, data, sizeof(response));
    if (response.requestId != pendingRequest || !rx) return;
    const int64_t t4 = esp_timer_get_time();
    const int64_t rtt = (t4 - pendingT1) - (response.t3 - response.t2);
    if (rtt < 0 || rtt > 200000) return;
    const int64_t measuredOffset =
        ((response.t2 - pendingT1) + (response.t3 - t4)) / 2;
    if (bestRttUs == INT64_MAX) offsetUs = measuredOffset;
    else offsetUs += (measuredOffset - offsetUs) / 4;
    if (rtt < bestRttUs) bestRttUs = rtt;
    lastRttUs = rtt;
    SyncCommit commit{MSG_SYNC_COMMIT, response.requestId, offsetUs};
    rx->writeValue((uint8_t *)&commit, sizeof(commit), false);
    lastSyncMs = millis();
    if (!synced) Serial.printf("[SYNC] BLE %s RTT:%lld OFFSET:%+lld SYNCED\n",
                               BLE_NODE_NAME, (long long)rtt, (long long)offsetUs);
    synced = true;
}

static void connectPending() {
    if (!pendingDevice) return;
    const NimBLEAdvertisedDevice *device = pendingDevice;
    pendingDevice = nullptr;
    if (!client) {
        client = NimBLEDevice::createClient();
        if (!client) return;
        client->setClientCallbacks(&clientCallbacks, false);
    }
    if (!client->connect(device, false)) return;
    NimBLERemoteService *service = client->getService(SERVICE_UUID);
    if (!service) { client->disconnect(); return; }
    rx = service->getCharacteristic(RX_UUID);
    tx = service->getCharacteristic(TX_UUID);
    if (!rx || !tx || !tx->subscribe(true, onNotify)) {
        client->disconnect(); return;
    }
    connected = true; synced = false; bestRttUs = INT64_MAX;
    lastSyncRequestMs = 0;
    Serial.printf("[BLE] %s connected\n", BLE_NODE_NAME);
}

void bleMasterBegin() {
    ackQueue = xQueueCreate(16, sizeof(BleLedAck));
    triggerQueue = xQueueCreate(BLE_TRIGGER_QUEUE_LENGTH, sizeof(BleTriggerPacket));
    NimBLEDevice::init("ECG_MASTER_WIFI_BLE");
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&scanCallbacks);
    scan->setActiveScan(true);
    scan->setDuplicateFilter(false);
    scan->setInterval(160);
    scan->setWindow(60);
    scan->start(5000, false);
    Serial.printf("[BLE] scanning only for %s\n", BLE_NODE_NAME);

    xTaskCreatePinnedToCore(
        [](void *) {
            for (;;) {
                bleMasterTick();
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        },
        "ble-master", 6144, nullptr, 2, &bleTaskHandle, 0);
    if (!bleTaskHandle) Serial.println("[BLE] task could not be created");
}

void bleMasterTick() {
    connectPending();
    NimBLEScan *scan = NimBLEDevice::getScan();
    if (!connected && !pendingDevice && !scan->isScanning()) scan->start(5000, false);
    const uint32_t nowMs = millis();
    if (synced && nowMs - lastSyncMs >= BLE_SYNC_TIMEOUT_MS) {
        synced = false; bestRttUs = INT64_MAX;
        Serial.println("[SYNC] BLE timeout");
    }
    if (connected && rx && nowMs - lastSyncRequestMs >= BLE_RESYNC_INTERVAL_MS) {
        pendingT1 = esp_timer_get_time();
        pendingRequest = ++nextRequest;
        lastSyncRequestMs = nowMs;
        SyncRequest request{MSG_SYNC_REQ, pendingRequest, pendingT1};
        rx->writeValue((uint8_t *)&request, sizeof(request), false);
    }

    BleTriggerPacket packet{};
    while (connected && synced && rx && triggerQueue &&
           xQueueReceive(triggerQueue, &packet, 0) == pdTRUE) {
        if (!rx->writeValue((uint8_t *)&packet, sizeof(packet), false)) {
            Serial.printf("[BLE] trigger #%lu send failed\n",
                          (unsigned long)packet.eventId);
        }
    }

    publishedConnected = connected;
    publishedSynced = connected && synced;
    publishedRttUs = lastRttUs > 0 ? (uint32_t)lastRttUs : 0;
}

BleNodeStatus bleNodeStatus() {
    return {publishedConnected, publishedSynced, publishedRttUs};
}

bool bleSendTrigger(const TriggerPacket &packet) {
    const uint64_t ownBit = maskForDevice(BLE_NODE_ID);
    if (packet.targetMask != 0 && (packet.targetMask & ownBit) == 0) return true;
    if (!publishedConnected || !publishedSynced || !triggerQueue) return false;
    BleTriggerPacket out{packet.type, packet.eventId, packet.pulseType,
                         packet.executeAtMasterUs, packet.ledOnTimeUs};
    return xQueueSend(triggerQueue, &out, 0) == pdTRUE;
}

bool bleTakeLedAck(BleLedAck &ack) {
    return ackQueue && xQueueReceive(ackQueue, &ack, 0) == pdTRUE;
}

uint32_t bleRecommendedLeadUs() {
    const uint32_t rttUs = publishedRttUs;
    if (!publishedSynced || rttUs == 0) return 200000;
    return min<uint32_t>(600000, 100000 + 2 * rttUs);
}
