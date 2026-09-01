#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_timer.h>
#include <string.h>

#include "config.h"
#include "protocol.h"

struct PendingPulse {
    uint32_t eventId;
    int64_t startLocalUs;
    int64_t endLocalUs;
};

static NimBLECharacteristic* txCharacteristic = nullptr;
static portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
static PendingPulse eventQueue[EVENT_QUEUE_SIZE];
static uint8_t eventCount = 0;
static bool clockSynchronized = false;
static int64_t clockOffsetUs = 0;
static uint32_t lastSyncRequestId = 0;
static bool ledIsOn = false;
static int64_t ledOffAtUs = 0;
static bool forceLedOff = false;

static constexpr uint8_t LED_INACTIVE_LEVEL =
    LED_ACTIVE_LEVEL == HIGH ? LOW : HIGH;

static void clearRuntimeState() {
    portENTER_CRITICAL(&stateMux);
    clockSynchronized = false;
    eventCount = 0;
    ledIsOn = false;
    ledOffAtUs = 0;
    forceLedOff = true;
    portEXIT_CRITICAL(&stateMux);
}

static void queuePulse(const TriggerPacket& packet) {
    portENTER_CRITICAL(&stateMux);
    if (!clockSynchronized || eventCount >= EVENT_QUEUE_SIZE) {
        portEXIT_CRITICAL(&stateMux);
        return;
    }
    for (uint8_t i = 0; i < eventCount; ++i) {
        if (eventQueue[i].eventId == packet.eventId) {
            portEXIT_CRITICAL(&stateMux);
            return;
        }
    }

    const int64_t startUs = packet.executeAtMasterUs + clockOffsetUs;
    uint8_t insertAt = eventCount;
    while (insertAt > 0 && eventQueue[insertAt - 1].startLocalUs > startUs) {
        eventQueue[insertAt] = eventQueue[insertAt - 1];
        --insertAt;
    }
    eventQueue[insertAt] = {packet.eventId, startUs,
                            startUs + packet.ledOnTimeUs};
    ++eventCount;
    portEXIT_CRITICAL(&stateMux);
}

static void notify(const void* packet, size_t size) {
    if (!txCharacteristic) return;
    txCharacteristic->setValue(static_cast<const uint8_t*>(packet), size);
    txCharacteristic->notify();
}

class RxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* characteristic,
                 NimBLEConnInfo&) override {
        const int64_t receivedAtUs = esp_timer_get_time();
        NimBLEAttValue value = characteristic->getValue();
        if (value.size() < 1) return;

        if (value.data()[0] == MSG_SYNC_REQ && value.size() == sizeof(SyncRequest)) {
            SyncRequest request{};
            memcpy(&request, value.data(), sizeof(request));
            portENTER_CRITICAL(&stateMux);
            lastSyncRequestId = request.requestId;
            portEXIT_CRITICAL(&stateMux);
            SyncResponse response{MSG_SYNC_RESP, request.requestId,
                                  receivedAtUs, esp_timer_get_time()};
            notify(&response, sizeof(response));
        } else if (value.data()[0] == MSG_SYNC_COMMIT &&
                   value.size() == sizeof(SyncCommit)) {
            SyncCommit commit{};
            memcpy(&commit, value.data(), sizeof(commit));
            portENTER_CRITICAL(&stateMux);
            if (commit.requestId == lastSyncRequestId) {
                clockOffsetUs = commit.offsetUs;
                clockSynchronized = true;
            }
            portEXIT_CRITICAL(&stateMux);
        } else if (value.data()[0] == MSG_TRIGGER &&
                   value.size() == sizeof(TriggerPacket)) {
            TriggerPacket packet{};
            memcpy(&packet, value.data(), sizeof(packet));
            queuePulse(packet);
        }
    }
};

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
        server->updateConnParams(info.getConnHandle(), 6, 12, 0, 400);
        Serial.println("[BLE] master connected");
    }

    void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
        clearRuntimeState();
        digitalWrite(LED_PIN, LED_INACTIVE_LEVEL);
        Serial.printf("[BLE] disconnected, reason=%d\n", reason);
        NimBLEDevice::startAdvertising();
    }
};

static RxCallbacks rxCallbacks;
static ServerCallbacks serverCallbacks;

static void startBle() {
    NimBLEDevice::init(NODE_NAME);
    NimBLEServer* server = NimBLEDevice::createServer();
    server->setCallbacks(&serverCallbacks);
    NimBLEService* service = server->createService(SERVICE_UUID);
    NimBLECharacteristic* rx = service->createCharacteristic(
        RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    txCharacteristic = service->createCharacteristic(
        TX_UUID, NIMBLE_PROPERTY::NOTIFY);
    rx->setCallbacks(&rxCallbacks);
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    NimBLEAdvertisementData data;
    data.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    data.addServiceUUID(SERVICE_UUID);
    advertising->setAdvertisementData(data);
    NimBLEAdvertisementData scanResponse;
    scanResponse.setName(NODE_NAME);
    advertising->setScanResponseData(scanResponse);
    advertising->start();
    Serial.printf("[BLE] advertising as %s\n", NODE_NAME);
}

static void ledTick() {
    const int64_t nowUs = esp_timer_get_time();
    uint32_t ackIds[EVENT_QUEUE_SIZE];
    uint8_t ackCount = 0;

    portENTER_CRITICAL(&stateMux);
    if (forceLedOff) {
        forceLedOff = false;
        ledIsOn = false;
    }
    while (eventCount > 0 && eventQueue[0].startLocalUs <= nowUs) {
        PendingPulse event = eventQueue[0];
        for (uint8_t i = 1; i < eventCount; ++i) eventQueue[i - 1] = eventQueue[i];
        --eventCount;
        if (event.endLocalUs > nowUs) {
            ledIsOn = true;
            if (event.endLocalUs > ledOffAtUs) ledOffAtUs = event.endLocalUs;
            ackIds[ackCount++] = event.eventId;
        }
    }
    if (ledIsOn && nowUs >= ledOffAtUs) ledIsOn = false;
    const bool outputOn = ledIsOn;
    portEXIT_CRITICAL(&stateMux);

    digitalWrite(LED_PIN, outputOn ? LED_ACTIVE_LEVEL : LED_INACTIVE_LEVEL);
    for (uint8_t i = 0; i < ackCount; ++i) {
        LedOnAck ack{MSG_LED_ON_ACK, ackIds[i], esp_timer_get_time()};
        notify(&ack, sizeof(ack));
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LED_INACTIVE_LEVEL);
    startBle();
}

void loop() {
    ledTick();
    delayMicroseconds(50);
}
