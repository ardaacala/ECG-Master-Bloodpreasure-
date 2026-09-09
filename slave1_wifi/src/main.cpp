#include <Arduino.h>
#include <WiFi.h>
#include <esp_timer.h>
#include <Preferences.h>
#include <cstring>

#include "config.h"
#include "protocol.h"

// ================= USER CONFIG =================
constexpr uint8_t DEVICE_ID = 1;
constexpr char DEVICE_NAME[] = "ECG_LEFT";
// ===============================================

static_assert(sizeof(DEVICE_NAME) <= DEVICE_NAME_MAX_LENGTH,
              "DEVICE_NAME is too long");

struct PendingPulse {
    uint32_t eventId;
    uint8_t pulseType;
    int64_t startLocalUs;
    int64_t endLocalUs;
    int64_t offsetUsedUs;
};

static WiFiClient masterClient;
static PendingPulse eventQueue[EVENT_QUEUE_SIZE];
static uint8_t eventCount = 0;
static bool clockSynchronized = false;
static int64_t clockOffsetUs = 0; // slave saati - master saati
static uint32_t lastSyncRequestId = 0;
static uint32_t lastSyncCommitMs = 0;
static uint32_t lastWifiAttemptMs = 0;
static uint32_t lastTcpAttemptMs = 0;
static bool ledIsOn = false;
static int64_t ledOffAtUs = 0;
static uint8_t rxBuffer[sizeof(TriggerPacket)] = {};
static size_t rxLength = 0;
static uint32_t lastStatusMs = 0;
static uint32_t lateDrops = 0;
static uint32_t queueRejects = 0;
static Preferences stateStorage;
static bool retainedStateValid = false;
static bool retainedStatePersisted = false;
static uint16_t retainedBpm = 72;
static PressureZone retainedZone = PRESSURE_BELOW;
static uint16_t retainedDelayMs = 0;
static int64_t nextStateBeatLocalUs = 0;
static constexpr uint8_t TRIGGER_STATE_PROTOCOL_VERSION = 3;
static constexpr int64_t STATE_PULSE_DURATION_US = 100000;

static constexpr uint8_t LED_INACTIVE_LEVEL =
    LED_ACTIVE_LEVEL == HIGH ? LOW : HIGH;

static void clearRuntimeState() {
    clockSynchronized = false;
    eventCount = 0;
    rxLength = 0;
}

static bool sendBytes(const void *data, size_t length) {
    return masterClient.connected() &&
           masterClient.write((const uint8_t *)data, length) == length;
}

static size_t incomingPacketSize(uint8_t type) {
    if (type == MSG_SYNC_REQ) return sizeof(SyncRequest);
    if (type == MSG_SYNC_COMMIT) return sizeof(SyncCommit);
    if (type == MSG_TRIGGER) return sizeof(TriggerPacket);
    if (type == MSG_TRIGGER_STATE) return sizeof(TriggerStatePacket);
    return 0;
}

static bool persistRetainedState() {
    if (!stateStorage.begin("trigger-v3", false)) return false;
    const bool ok = stateStorage.putUShort("bpm", retainedBpm) == sizeof(uint16_t) &&
                    stateStorage.putUChar("zone", (uint8_t)retainedZone) == sizeof(uint8_t) &&
                    stateStorage.putUShort("delay", retainedDelayMs) == sizeof(uint16_t) &&
                    stateStorage.putBool("valid", true) == sizeof(bool);
    stateStorage.end();
    return ok;
}

static void scheduleRetainedState(const TriggerStatePacket *packet = nullptr) {
    if (!retainedStateValid || retainedZone != PRESSURE_INSIDE) {
        nextStateBeatLocalUs = 0;
        ledIsOn = false;
        ledOffAtUs = 0;
        return;
    }
    const int64_t nowUs = esp_timer_get_time();
    const int64_t periodUs = 60000000LL / retainedBpm;
    int64_t firstUs = packet && clockSynchronized
        ? packet->effectiveAtMasterUs + clockOffsetUs +
              (int64_t)retainedDelayMs * 1000
        : nowUs + (int64_t)retainedDelayMs * 1000;
    if (firstUs <= nowUs) {
        firstUs += ((nowUs - firstUs) / periodUs + 1) * periodUs;
    }
    nextStateBeatLocalUs = firstUs;
}

static void loadRetainedState() {
    if (!stateStorage.begin("trigger-v3", true)) return;
    retainedStateValid = stateStorage.getBool("valid", false);
    retainedBpm = stateStorage.getUShort("bpm", 72);
    retainedZone = (PressureZone)stateStorage.getUChar("zone", PRESSURE_BELOW);
    retainedDelayMs = stateStorage.getUShort("delay", 0);
    stateStorage.end();
    if (retainedBpm < 30 || retainedBpm > 240 ||
        retainedZone > PRESSURE_ABOVE) {
        retainedStateValid = false;
    } else if (retainedStateValid) {
        retainedStatePersisted = true;
    }
    scheduleRetainedState();
}

static void applyTriggerState(const TriggerStatePacket &packet) {
    TriggerStateApplyResult result = TRIGGER_STATE_INVALID;
    if (packet.protocolVersion == TRIGGER_STATE_PROTOCOL_VERSION &&
        (packet.receiverDeviceId == 0 || packet.receiverDeviceId == DEVICE_ID) &&
        packet.bpm >= 30 && packet.bpm <= 240 &&
        packet.pressureZone <= PRESSURE_ABOVE &&
        packet.actionDelayMs <= 60000) {
        const bool changed = !retainedStateValid || !retainedStatePersisted ||
            retainedBpm != packet.bpm ||
            retainedZone != (PressureZone)packet.pressureZone ||
            retainedDelayMs != packet.actionDelayMs;
        retainedStateValid = true;
        retainedBpm = packet.bpm;
        retainedZone = (PressureZone)packet.pressureZone;
        retainedDelayMs = packet.actionDelayMs;
        eventCount = 0;
        scheduleRetainedState(&packet);
        if (changed) {
            retainedStatePersisted = persistRetainedState();
            result = retainedStatePersisted ? TRIGGER_STATE_APPLIED
                                            : TRIGGER_STATE_PERSIST_FAILED;
        } else {
            result = TRIGGER_STATE_UNCHANGED;
        }
    }
    TriggerStateAck ack{MSG_TRIGGER_STATE_ACK,
                        TRIGGER_STATE_PROTOCOL_VERSION, DEVICE_ID,
                        packet.stateRevision, (uint8_t)result};
    sendBytes(&ack, sizeof(ack));
    Serial.printf("[STATE] rev=%lu bpm=%u zone=%u delay=%u result=%u\n",
                  (unsigned long)packet.stateRevision, packet.bpm,
                  packet.pressureZone, packet.actionDelayMs, result);
}

static bool queuePulse(const TriggerPacket &packet) {
    if (!clockSynchronized || eventCount >= EVENT_QUEUE_SIZE ||
        (packet.pulseType != PULSE_SMALL && packet.pulseType != PULSE_LARGE)) {
        ++queueRejects;
        return false;
    }
    for (uint8_t index = 0; index < eventCount; ++index) {
        if (eventQueue[index].eventId == packet.eventId) return false;
    }
    const int64_t localStartUs = packet.executeAtMasterUs + clockOffsetUs;
    if (localStartUs <= esp_timer_get_time()) {
        ++lateDrops;
        return false;
    }
    uint8_t insertAt = eventCount;
    while (insertAt > 0 && eventQueue[insertAt - 1].startLocalUs > localStartUs) {
        eventQueue[insertAt] = eventQueue[insertAt - 1];
        --insertAt;
    }
    eventQueue[insertAt] = {packet.eventId, packet.pulseType, localStartUs,
                            localStartUs + packet.ledOnTimeUs, clockOffsetUs};
    ++eventCount;
    return true;
}

static void handlePacket(const uint8_t *data, size_t length, int64_t receivedAtUs) {
    if (data[0] == MSG_SYNC_REQ && length == sizeof(SyncRequest)) {
        SyncRequest request{};
        memcpy(&request, data, sizeof(request));
        lastSyncRequestId = request.requestId;
        // Master offset'i yalnizca daha iyi bir RTT ornegi buldugunda yeniden
        // commit edebilir. Her gecerli sync istegi mevcut oturumu canli tutar.
        if (clockSynchronized) lastSyncCommitMs = millis();
        SyncResponse response{MSG_SYNC_RESP, request.requestId, receivedAtUs,
                              esp_timer_get_time()};
        if (!sendBytes(&response, sizeof(response))) {
            Serial.println("[SYNC] response failed");
        }
        return;
    }
    if (data[0] == MSG_SYNC_COMMIT && length == sizeof(SyncCommit)) {
        SyncCommit commit{};
        memcpy(&commit, data, sizeof(commit));
        if (commit.requestId != lastSyncRequestId) return;
        clockOffsetUs = commit.offsetUs;
        clockSynchronized = true;
        lastSyncCommitMs = millis();
        Serial.printf("[SYNC] ready offset=%+lld us\n", (long long)clockOffsetUs);
        return;
    }
    if (data[0] == MSG_TRIGGER && length == sizeof(TriggerPacket)) {
        TriggerPacket packet{};
        memcpy(&packet, data, sizeof(packet));
        if (packet.targetMask != 0) {
            const uint64_t ownBit = maskForDevice(DEVICE_ID);
            if (ownBit == 0 || (packet.targetMask & ownBit) == 0) return;
        }
        if (!queuePulse(packet)) {
            Serial.printf("[EVENT] #%lu rejected\n", (unsigned long)packet.eventId);
        }
        return;
    }
    if (data[0] == MSG_TRIGGER_STATE &&
        length == sizeof(TriggerStatePacket)) {
        TriggerStatePacket packet{};
        memcpy(&packet, data, sizeof(packet));
        applyTriggerState(packet);
    }
}

static void networkReadTick() {
    while (masterClient.available()) {
        int64_t receivedAtUs = esp_timer_get_time();
        if (rxLength == 0) {
            const int value = masterClient.read();
            if (value < 0) return;
            if (!incomingPacketSize((uint8_t)value)) continue;
            rxBuffer[0] = (uint8_t)value;
            rxLength = 1;
        }
        const size_t packetSize = incomingPacketSize(rxBuffer[0]);
        while (masterClient.available() && rxLength < packetSize) {
            rxBuffer[rxLength++] = (uint8_t)masterClient.read();
        }
        if (rxLength != packetSize) return;
        handlePacket(rxBuffer, packetSize, receivedAtUs);
        rxLength = 0;
    }
}

static void connectionTick() {
    const uint32_t nowMs = millis();
    if (WiFi.status() != WL_CONNECTED) {
        if (masterClient.connected()) masterClient.stop();
        clearRuntimeState();
        if (nowMs - lastWifiAttemptMs >= WIFI_RETRY_INTERVAL_MS) {
            lastWifiAttemptMs = nowMs;
            Serial.printf("[WIFI] connecting to %s\n", WIFI_SSID);
            WiFi.disconnect();
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        }
        return;
    }
    if (!masterClient.connected()) {
        clearRuntimeState();
        if (nowMs - lastTcpAttemptMs < TCP_RETRY_INTERVAL_MS) return;
        lastTcpAttemptMs = nowMs;
        IPAddress masterIp(MASTER_IP_1, MASTER_IP_2, MASTER_IP_3, MASTER_IP_4);
        Serial.printf("[TCP] connecting to %s:%u\n",
                      masterIp.toString().c_str(), MASTER_TCP_PORT);
        if (masterClient.connect(masterIp, MASTER_TCP_PORT)) {
            masterClient.setNoDelay(true);
            HelloPacket hello{};
            hello.type = MSG_HELLO;
            hello.deviceId = DEVICE_ID;
            strncpy(hello.deviceName, DEVICE_NAME, sizeof(hello.deviceName) - 1);
            sendBytes(&hello, sizeof(hello));
            Serial.printf("[TCP] master connected, local IP: %s\n",
                          WiFi.localIP().toString().c_str());
            lastStatusMs = nowMs - 3000;
        }
        return;
    }
    if (clockSynchronized && nowMs - lastSyncCommitMs >= SYNC_TIMEOUT_MS) {
        Serial.println("[SYNC] timeout - reconnecting TCP");
        // Master resetlendiginde eski TCP soketi istemci tarafinda bir sure
        // bagli gorunebilir. Soketi kapatmak yeni master sunucusuna hizli ve
        // temiz bir baglanti kurulmasini saglar.
        masterClient.stop();
        clearRuntimeState();
        lastTcpAttemptMs = nowMs - TCP_RETRY_INTERVAL_MS;
        return;
    }
    networkReadTick();
    if (nowMs - lastStatusMs >= 3000) {
        lastStatusMs = nowMs;
        DeviceStatusPacket status{MSG_DEVICE_STATUS, DEVICE_ID,
                                  (int8_t)WiFi.RSSI(), lateDrops, queueRejects};
        sendBytes(&status, sizeof(status));
    }
}

static void ledTick() {
    const int64_t nowUs = esp_timer_get_time();
    uint32_t ackIds[EVENT_QUEUE_SIZE];
    int32_t ackLateness[EVENT_QUEUE_SIZE];
    int64_t ackMasterTimes[EVENT_QUEUE_SIZE];
    uint8_t ackCount = 0;
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
        const PendingPulse event = eventQueue[0];
        for (uint8_t index = 1; index < eventCount; ++index) {
            eventQueue[index - 1] = eventQueue[index];
        }
        --eventCount;
        if (event.endLocalUs <= nowUs) {
            ++lateDrops;
            continue;
        }
        ledIsOn = true;
        if (event.endLocalUs > ledOffAtUs) ledOffAtUs = event.endLocalUs;
        if (ackCount < EVENT_QUEUE_SIZE) {
            ackIds[ackCount] = event.eventId;
            ackLateness[ackCount] = (int32_t)(nowUs - event.startLocalUs);
            ackMasterTimes[ackCount] = nowUs;
            ++ackCount;
        }
    }
    if (ledIsOn && nowUs >= ledOffAtUs) ledIsOn = false;
    digitalWrite(LED_PIN, ledIsOn ? LED_ACTIVE_LEVEL : LED_INACTIVE_LEVEL);
    for (uint8_t index = 0; index < ackCount; ++index) {
        LedOnAck ack{MSG_LED_ON_ACK, ackIds[index], ackLateness[index],
                     ackMasterTimes[index]};
        sendBytes(&ack, sizeof(ack));
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);
    if (ONBOARD_LED_PIN >= 0 && ONBOARD_LED_PIN != LED_PIN) {
        pinMode(ONBOARD_LED_PIN, INPUT);
    }
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LED_INACTIVE_LEVEL);
    loadRetainedState();
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    Serial.printf("ECG WIFI SLAVE (ID:%u, NAME:%s) start, LED GPIO:%d active:%s\n",
                  DEVICE_ID, DEVICE_NAME, LED_PIN,
                  LED_ACTIVE_LEVEL == HIGH ? "HIGH" : "LOW");
    // Ilk baglanti denemesinin hemen yapilmasini sagla.
    lastWifiAttemptMs = millis() - WIFI_RETRY_INTERVAL_MS;
}

void loop() {
    connectionTick();
    ledTick();
    delayMicroseconds(50);
}
