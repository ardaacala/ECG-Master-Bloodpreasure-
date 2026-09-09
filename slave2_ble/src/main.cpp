#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_timer.h>
#include <Preferences.h>
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
static Preferences stateStorage;
static bool retainedStateValid = false;
static bool retainedStatePersisted = false;
static uint16_t retainedBpm = 72;
static PressureZone retainedZone = PRESSURE_BELOW;
static uint16_t retainedDelayMs = 0;
static int64_t nextStateBeatLocalUs = 0;
static constexpr uint16_t DEVICE_ID = 2;
static constexpr uint8_t TRIGGER_STATE_PROTOCOL_VERSION = 3;
static constexpr int64_t STATE_PULSE_DURATION_US = 100000;

static constexpr uint8_t LED_INACTIVE_LEVEL =
    LED_ACTIVE_LEVEL == HIGH ? LOW : HIGH;

static void notify(const void* packet, size_t size);

static void clearRuntimeState() {
    portENTER_CRITICAL(&stateMux);
    clockSynchronized = false;
    eventCount = 0;
    portEXIT_CRITICAL(&stateMux);
}

static bool persistRetainedState(uint16_t bpm, PressureZone zone,
                                 uint16_t delayMs) {
    if (!stateStorage.begin("trigger-v3", false)) return false;
    const bool ok = stateStorage.putUShort("bpm", bpm) == sizeof(uint16_t) &&
                    stateStorage.putUChar("zone", (uint8_t)zone) == sizeof(uint8_t) &&
                    stateStorage.putUShort("delay", delayMs) == sizeof(uint16_t) &&
                    stateStorage.putBool("valid", true) == sizeof(bool);
    stateStorage.end();
    return ok;
}

static void setRetainedState(uint16_t bpm, PressureZone zone,
                             uint16_t delayMs,
                             const TriggerStatePacket *packet = nullptr) {
    const int64_t nowUs = esp_timer_get_time();
    portENTER_CRITICAL(&stateMux);
    retainedStateValid = true;
    retainedBpm = bpm;
    retainedZone = zone;
    retainedDelayMs = delayMs;
    eventCount = 0;
    if (zone == PRESSURE_INSIDE) {
        const int64_t periodUs = 60000000LL / bpm;
        int64_t firstUs = packet && clockSynchronized
            ? packet->effectiveAtMasterUs + clockOffsetUs +
                  (int64_t)delayMs * 1000
            : nowUs + (int64_t)delayMs * 1000;
        if (firstUs <= nowUs) {
            firstUs += ((nowUs - firstUs) / periodUs + 1) * periodUs;
        }
        nextStateBeatLocalUs = firstUs;
    } else {
        nextStateBeatLocalUs = 0;
        ledIsOn = false;
        ledOffAtUs = 0;
        forceLedOff = true;
    }
    portEXIT_CRITICAL(&stateMux);
}

static void loadRetainedState() {
    if (!stateStorage.begin("trigger-v3", true)) return;
    const bool valid = stateStorage.getBool("valid", false);
    const uint16_t bpm = stateStorage.getUShort("bpm", 72);
    const PressureZone zone =
        (PressureZone)stateStorage.getUChar("zone", PRESSURE_BELOW);
    const uint16_t delayMs = stateStorage.getUShort("delay", 0);
    stateStorage.end();
    if (valid && bpm >= 30 && bpm <= 240 && zone <= PRESSURE_ABOVE) {
        setRetainedState(bpm, zone, delayMs);
        retainedStatePersisted = true;
    }
}

static void applyTriggerState(const TriggerStatePacket &packet) {
    TriggerStateApplyResult result = TRIGGER_STATE_INVALID;
    if (packet.protocolVersion == TRIGGER_STATE_PROTOCOL_VERSION &&
        (packet.receiverDeviceId == 0 || packet.receiverDeviceId == DEVICE_ID) &&
        packet.bpm >= 30 && packet.bpm <= 240 &&
        packet.pressureZone <= PRESSURE_ABOVE &&
        packet.actionDelayMs <= 60000) {
        portENTER_CRITICAL(&stateMux);
        const bool changed = !retainedStateValid || !retainedStatePersisted ||
            retainedBpm != packet.bpm ||
            retainedZone != (PressureZone)packet.pressureZone ||
            retainedDelayMs != packet.actionDelayMs;
        portEXIT_CRITICAL(&stateMux);
        setRetainedState(packet.bpm, (PressureZone)packet.pressureZone,
                         packet.actionDelayMs, &packet);
        if (changed) {
            retainedStatePersisted = persistRetainedState(
                packet.bpm, (PressureZone)packet.pressureZone,
                packet.actionDelayMs);
            result = retainedStatePersisted ? TRIGGER_STATE_APPLIED
                                            : TRIGGER_STATE_PERSIST_FAILED;
        } else {
            result = TRIGGER_STATE_UNCHANGED;
        }
    }
    TriggerStateAck ack{MSG_TRIGGER_STATE_ACK,
                        TRIGGER_STATE_PROTOCOL_VERSION, DEVICE_ID,
                        packet.stateRevision, (uint8_t)result};
    notify(&ack, sizeof(ack));
    Serial.printf("[STATE] rev=%lu bpm=%u zone=%u delay=%u result=%u\n",
                  (unsigned long)packet.stateRevision, packet.bpm,
                  packet.pressureZone, packet.actionDelayMs, result);
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
        } else if (value.data()[0] == MSG_TRIGGER_STATE &&
                   value.size() == sizeof(TriggerStatePacket)) {
            TriggerStatePacket packet{};
            memcpy(&packet, value.data(), sizeof(packet));
            applyTriggerState(packet);
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
    if (retainedStateValid && retainedZone == PRESSURE_INSIDE &&
        nextStateBeatLocalUs > 0 && nowUs >= nextStateBeatLocalUs) {
        const int64_t periodUs = 60000000LL / retainedBpm;
        const int64_t periods = (nowUs - nextStateBeatLocalUs) / periodUs + 1;
        nextStateBeatLocalUs += periods * periodUs;
        ledIsOn = true;
        ledOffAtUs = nowUs + STATE_PULSE_DURATION_US;
    } else if (retainedStateValid && retainedZone != PRESSURE_INSIDE) {
        ledIsOn = false;
        ledOffAtUs = 0;
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
    loadRetainedState();
    startBle();
}

void loop() {
    ledTick();
    delayMicroseconds(50);
}
