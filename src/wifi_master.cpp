#include "wifi_master.h"
#include "config.h"
#include "ui.h"
#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

struct ClientRuntime {
    WiFiClient client;
    bool active;
    bool synced;
    uint8_t deviceId;
    char deviceName[DEVICE_NAME_MAX_LENGTH];
    int64_t clockOffsetUs;
    int64_t bestRttUs;
    int64_t lastRttUs;
    int64_t jitterUs;
    int64_t pendingT1;
    uint32_t pendingRequest;
    uint32_t lastSyncMs;
    uint32_t lastSyncRequestMs;
    uint8_t syncSamples;
    int8_t rssiDbm;
    uint32_t lastStatusMs;
    uint32_t lateDrops;
    uint32_t queueRejects;
    uint8_t quality;
    uint8_t rxBuffer[sizeof(HelloPacket)];
    size_t rxLength;
};

static WiFiServer server(WIFI_TCP_PORT);
static ClientRuntime clients[MAX_WIFI_CLIENTS] = {};
// DEVICE_ID tam 8-bit tutulur. Routing socket sirasi/IP/MAC yerine bu tabloyla
// yapilir. 65..255 kimlikleri gelecekteki protokol genislemesine hazirdir;
// mevcut uint64_t targetMask sadece 1..64 kimliklerini tekil hedefleyebilir.
static ClientRuntime *deviceMap[UINT8_MAX + 1] = {};
static uint32_t nextRequestId = 0;
static QueueHandle_t triggerQueue = nullptr;
static QueueHandle_t ackQueue = nullptr;
static TaskHandle_t networkTaskHandle = nullptr;
static volatile uint8_t publishedConnected = 0;
static volatile uint8_t publishedSynced = 0;
static volatile uint32_t publishedLeadUs = 200000;
static volatile uint32_t publishedRttUs = 0;
static volatile uint32_t publishedJitterUs = 0;
static volatile int32_t publishedSpreadUs = 0;
static volatile bool publishedHasSpread = false;
static volatile int8_t publishedWorstRssi = 0;
static volatile uint8_t publishedQuality = 3;
static volatile uint32_t publishedSent = 0;
static volatile uint32_t publishedAcked = 0;
static volatile uint32_t publishedMissed = 0;

static constexpr uint8_t TRIGGER_QUEUE_LENGTH = 16;
static constexpr int64_t MAX_VALID_RTT_US = 200000;
static constexpr uint32_t MIN_LEAD_US = 200000;
static constexpr uint32_t MAX_LEAD_US = 600000;
static constexpr uint32_t STATUS_TIMEOUT_MS = 10000;
static uint32_t totalSent = 0;
static uint32_t totalAcked = 0;
static uint32_t totalMissed = 0;

struct EventAudit {
    uint32_t eventId;
    int64_t minActualMasterUs;
    int64_t maxActualMasterUs;
    uint8_t acknowledgements;
    uint64_t expectedMask;
    uint64_t ackMask;
    int64_t deadlineUs;
    bool finalized;
};
static EventAudit audits[32] = {};

static EventAudit &auditFor(uint32_t eventId) {
    EventAudit &audit = audits[eventId % 32];
    if (audit.eventId != eventId) {
        audit = {eventId, INT64_MAX, INT64_MIN, 0, 0, 0, 0, false};
    }
    return audit;
}

static size_t incomingPacketSize(uint8_t type) {
    if (type == MSG_SYNC_RESP) return sizeof(SyncResponse);
    if (type == MSG_LED_ON_ACK) return sizeof(LedOnAck);
    if (type == MSG_HELLO) return sizeof(HelloPacket);
    if (type == MSG_DEVICE_STATUS) return sizeof(DeviceStatusPacket);
    return 0;
}

static bool sendBytes(ClientRuntime &node, const void *data, size_t length) {
    return node.active && node.client.connected() &&
           node.client.write((const uint8_t *)data, length) == length;
}

static void disconnectClient(uint8_t index) {
    ClientRuntime &node = clients[index];
    if (node.active) Serial.printf("[WIFI] device %u disconnected\n", index + 1);
    if (node.deviceId != 0 && deviceMap[node.deviceId] == &node) {
        deviceMap[node.deviceId] = nullptr;
    }
    node.client.stop();
    node = ClientRuntime{};
}

static void handlePacket(uint8_t index, const uint8_t *data, size_t length) {
    ClientRuntime &node = clients[index];
    if (data[0] == MSG_HELLO && length == sizeof(HelloPacket)) {
        HelloPacket hello{};
        memcpy(&hello, data, sizeof(hello));
        hello.deviceName[DEVICE_NAME_MAX_LENGTH - 1] = '\0';
        if (hello.deviceId == 0) {
            Serial.printf("[IDENTITY ERROR] connection %u sent invalid DEVICE_ID 0; rejected\n",
                          index + 1);
            disconnectClient(index);
            return;
        }
        ClientRuntime *existing = deviceMap[hello.deviceId];
        if (existing && existing != &node && existing->active &&
            existing->client.connected()) {
            Serial.printf("[DUPLICATE DEVICE_ID] ID:%u NAME:%s conflicts with active NAME:%s; new connection rejected\n",
                          hello.deviceId, hello.deviceName,
                          existing->deviceName);
            disconnectClient(index);
            return;
        }
        if (node.deviceId != 0 && node.deviceId != hello.deviceId &&
            deviceMap[node.deviceId] == &node) {
            deviceMap[node.deviceId] = nullptr;
        }
        node.deviceId = hello.deviceId;
        strncpy(node.deviceName, hello.deviceName, sizeof(node.deviceName) - 1);
        node.deviceName[sizeof(node.deviceName) - 1] = '\0';
        deviceMap[node.deviceId] = &node;
        Serial.printf("[DEVICE] ID:%u NAME:%s registered (transport slot %u)\n",
                      node.deviceId, node.deviceName, index + 1);
        return;
    }
    if (data[0] == MSG_LED_ON_ACK && length == sizeof(LedOnAck)) {
        LedOnAck ack{};
        memcpy(&ack, data, sizeof(ack));
        EventAudit &audit = auditFor(ack.eventId);
        const int64_t estimatedMasterUs = ack.actualLocalUs - node.clockOffsetUs;
        if (node.deviceId == WIFI_NODE_ID && ackQueue) {
            WifiLedAck mixedAck{ack.eventId, estimatedMasterUs};
            xQueueSend(ackQueue, &mixedAck, 0);
        }
        if (estimatedMasterUs < audit.minActualMasterUs) audit.minActualMasterUs = estimatedMasterUs;
        if (estimatedMasterUs > audit.maxActualMasterUs) audit.maxActualMasterUs = estimatedMasterUs;
        const uint64_t ackBit = maskForDevice(node.deviceId);
        if (ackBit != 0 && (audit.ackMask & ackBit) == 0) {
            audit.ackMask |= ackBit;
            ++audit.acknowledgements;
            ++totalAcked;
        }
        const int64_t spread = audit.maxActualMasterUs - audit.minActualMasterUs;
        if (audit.acknowledgements >= 2) {
            publishedSpreadUs = (int32_t)spread;
            publishedHasSpread = true;
        }
#if ENABLE_VERBOSE_TIMING_LOGS
        if (Serial.availableForWrite() >= 96) {
            Serial.printf("[TRIGGER CHECK] #%lu node:%u late:%+ldus spread:%ldus ack:%u\n",
                          (unsigned long)ack.eventId, node.deviceId,
                          (long)ack.latenessUs, (long)spread,
                          audit.acknowledgements);
        }
#endif
        return;
    }
    if (data[0] == MSG_DEVICE_STATUS && length == sizeof(DeviceStatusPacket)) {
        DeviceStatusPacket status{};
        memcpy(&status, data, sizeof(status));
        if (status.deviceId != node.deviceId) return;
        node.rssiDbm = status.rssiDbm;
        node.lateDrops = status.lateDrops;
        node.queueRejects = status.queueRejects;
        node.lastStatusMs = millis();
        return;
    }
    if (data[0] != MSG_SYNC_RESP || length != sizeof(SyncResponse)) return;
    SyncResponse response{};
    memcpy(&response, data, sizeof(response));
    if (response.requestId != node.pendingRequest) return;
    const int64_t t4 = esp_timer_get_time();
    const int64_t rtt = (t4 - node.pendingT1) - (response.t3 - response.t2);
    const int64_t offset = ((response.t2 - node.pendingT1) + (response.t3 - t4)) / 2;
    if (rtt >= 0 && rtt <= MAX_VALID_RTT_US) {
        if (node.bestRttUs == INT64_MAX) {
            node.bestRttUs = rtt;
            node.clockOffsetUs = offset;
            node.jitterUs = 0;
        } else {
            const int64_t rttDelta = llabs(rtt - node.lastRttUs);
            node.jitterUs = (node.jitterUs * 3 + rttDelta) / 4;
            // Ani olcum gurultusunun slave saatini sicratarak tetikleri
            // kaydirmamasi icin offset'i yumusak olarak takip et.
            node.clockOffsetUs += (offset - node.clockOffsetUs) / 4;
            if (rtt < node.bestRttUs) node.bestRttUs = rtt;
        }
        node.lastRttUs = rtt;
        SyncCommit commit{MSG_SYNC_COMMIT, response.requestId, node.clockOffsetUs};
        sendBytes(node, &commit, sizeof(commit));
    } else {
        return;
    }
    node.lastSyncMs = millis();
    const bool wasSynced = node.synced;
    if (++node.syncSamples >= SYNC_SAMPLE_COUNT) node.synced = true;
    if (!wasSynced && node.synced) {
        Serial.printf("[SYNC] device %u RTT:%lld OFFSET:%+lld SYNCED\n", index + 1,
                      (long long)rtt, (long long)node.clockOffsetUs);
    }
    // Monitor aciksa her periyodik kontrolde olculen saat farkini goster.
    // Monitor kapaliyken USB seri tamponunu doldurup ag gorevini bloke etme.
#if ENABLE_VERBOSE_TIMING_LOGS
    if (wasSynced && Serial.availableForWrite() >= 96) {
        Serial.printf("[SYNC CHECK] node:%u RTT:%lldus JITTER:%lldus OFFSET:%+lldus\n",
                      node.deviceId, (long long)rtt, (long long)node.jitterUs,
                      (long long)node.clockOffsetUs);
    }
#endif
}

static void readClient(uint8_t index) {
    ClientRuntime &node = clients[index];
    while (node.client.available()) {
        if (node.rxLength == 0) {
            const int value = node.client.read();
            if (value < 0) return;
            const size_t packetSize = incomingPacketSize((uint8_t)value);
            if (!packetSize) continue;
            node.rxBuffer[0] = (uint8_t)value;
            node.rxLength = 1;
        }
        const size_t packetSize = incomingPacketSize(node.rxBuffer[0]);
        while (node.client.available() && node.rxLength < packetSize) {
            node.rxBuffer[node.rxLength++] = (uint8_t)node.client.read();
        }
        if (node.rxLength == packetSize) {
            handlePacket(index, node.rxBuffer, packetSize);
            node.rxLength = 0;
        } else {
            return;
        }
    }
}

static void acceptClients() {
    WiFiClient incoming = server.accept();
    if (!incoming) return;
    for (uint8_t index = 0; index < MAX_WIFI_CLIENTS; ++index) {
        if (clients[index].active) continue;
        clients[index] = ClientRuntime{};
        clients[index].client = incoming;
        clients[index].active = true;
        clients[index].bestRttUs = INT64_MAX;
        clients[index].client.setNoDelay(true);
        Serial.printf("[WIFI] device %u connected: %s\n", index + 1,
                      clients[index].client.remoteIP().toString().c_str());
        return;
    }
    Serial.println("[WIFI] client limit reached, connection rejected");
    incoming.stop();
}

static void sendSync(uint8_t index, uint32_t nowMs) {
    ClientRuntime &node = clients[index];
    node.pendingT1 = esp_timer_get_time();
    node.pendingRequest = ++nextRequestId;
    node.lastSyncRequestMs = nowMs;
    SyncRequest request{MSG_SYNC_REQ, node.pendingRequest, node.pendingT1};
    if (!sendBytes(node, &request, sizeof(request))) disconnectClient(index);
}

void wifiMasterBegin() {
    WiFi.mode(WIFI_AP);
    if (!WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD, 1, false, MAX_WIFI_CLIENTS)) {
        Serial.println("[WIFI] access point could not start");
        return;
    }
    server.begin();
    server.setNoDelay(true);
    triggerQueue = xQueueCreate(TRIGGER_QUEUE_LENGTH, sizeof(TriggerPacket));
    ackQueue = xQueueCreate(16, sizeof(WifiLedAck));
    if (!triggerQueue) {
        Serial.println("[WIFI] trigger queue could not be created");
        return;
    }
    Serial.printf("[WIFI] SSID:%s IP:%s TCP:%u max:%u\n", WIFI_AP_SSID,
                  WiFi.softAPIP().toString().c_str(), WIFI_TCP_PORT, MAX_WIFI_CLIENTS);

    xTaskCreatePinnedToCore(
        [](void *) {
            for (;;) {
                wifiMasterTick();
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        },
        "wifi-sync", 6144, nullptr, 2, &networkTaskHandle, 0);
    if (!networkTaskHandle) Serial.println("[WIFI] network task could not be created");
}

void wifiMasterTick() {
    acceptClients();
    const uint32_t nowMs = millis();
    uint8_t connected = 0;
    uint8_t synced = 0;
    uint32_t recommendedLeadUs = MIN_LEAD_US;
    uint32_t worstRttUs = 0;
    uint32_t worstJitterUs = 0;
    int8_t worstRssi = 0;
    uint8_t systemQuality = 0;
    for (uint8_t index = 0; index < MAX_WIFI_CLIENTS; ++index) {
        ClientRuntime &node = clients[index];
        if (!node.active) continue;
        if (!node.client.connected()) {
            disconnectClient(index);
            continue;
        }
        ++connected;
        readClient(index);
        // readClient() bir sync cevabi islerken lastSyncMs, dongunun basinda
        // alinan nowMs'den birkac ms ileride olabilir. Eski nowMs ile cikarma
        // uint32_t tasmasi yapip anlik sahte timeout uretiyordu.
        const uint32_t clientNowMs = millis();
        if (node.synced && clientNowMs - node.lastSyncMs >= SYNC_TIMEOUT_MS) {
            node.synced = false;
            node.syncSamples = 0;
            node.bestRttUs = INT64_MAX;
            Serial.printf("[SYNC] device %u timeout\n", index + 1);
        }
        if (clientNowMs - node.lastSyncRequestMs >= RESYNC_INTERVAL_MS) {
            sendSync(index, clientNowMs);
        }
        if (node.synced && node.deviceId != 0) {
            uint8_t quality = 0; // 0 iyi, 1 orta, 2 zayif, 3 guvensiz
            if (node.lastRttUs >= 150000 || node.jitterUs >= 50000 ||
                (node.rssiDbm != 0 && node.rssiDbm < -85) ||
                (node.lastStatusMs != 0 && clientNowMs - node.lastStatusMs >= STATUS_TIMEOUT_MS)) {
                quality = 3;
            } else if (node.lastRttUs >= 75000 || node.jitterUs >= 30000 ||
                       (node.rssiDbm != 0 && node.rssiDbm < -78)) {
                quality = 2;
            } else if (node.lastRttUs >= 30000 || node.jitterUs >= 10000 ||
                       (node.rssiDbm != 0 && node.rssiDbm < -67)) {
                quality = 1;
            }
            node.quality = quality;
            if (quality > systemQuality) systemQuality = quality;
            if (quality < 3) ++synced;
            const int64_t qualityUs = node.lastRttUs + node.jitterUs;
            const uint32_t candidate = (uint32_t)min<int64_t>(
                MAX_LEAD_US, 100000 + 2 * max<int64_t>(0, qualityUs));
            if (candidate > recommendedLeadUs) recommendedLeadUs = candidate;
            if ((uint32_t)node.lastRttUs > worstRttUs) worstRttUs = (uint32_t)node.lastRttUs;
            if ((uint32_t)node.jitterUs > worstJitterUs) worstJitterUs = (uint32_t)node.jitterUs;
            if (node.rssiDbm != 0 && (worstRssi == 0 || node.rssiDbm < worstRssi)) {
                worstRssi = node.rssiDbm;
            }
        }
    }

    const int64_t nowUs = esp_timer_get_time();
    for (auto &audit : audits) {
        if (audit.eventId == 0 || audit.finalized || audit.deadlineUs == 0 ||
            nowUs < audit.deadlineUs) continue;
        audit.finalized = true;
        uint64_t missing = audit.expectedMask & ~audit.ackMask;
        while (missing) {
            totalMissed += (missing & 1ULL) != 0;
            missing >>= 1;
        }
    }

    TriggerPacket packet{};
    while (triggerQueue && xQueueReceive(triggerQueue, &packet, 0) == pdTRUE) {
        auditFor(packet.eventId);
        uint8_t sent = 0;
        uint64_t expectedMask = 0;
        if (packet.targetMask == 0) {
            for (auto &node : clients) {
                if (node.active && node.synced && node.quality < 3 &&
                    node.deviceId == WIFI_NODE_ID &&
                    node.client.connected() &&
                    sendBytes(node, &packet, sizeof(packet))) {
                    ++sent;
                    expectedMask |= maskForDevice(node.deviceId);
                }
            }
        } else {
            for (uint8_t deviceId = 1; deviceId <= 64; ++deviceId) {
                if ((packet.targetMask & maskForDevice(deviceId)) == 0) continue;
                ClientRuntime *node = deviceMap[deviceId];
                if (node && node->active && node->synced && node->quality < 3 &&
                    node->client.connected() &&
                    sendBytes(*node, &packet, sizeof(packet))) {
                    ++sent;
                    expectedMask |= maskForDevice(deviceId);
                }
            }
        }
        EventAudit &audit = auditFor(packet.eventId);
        audit.expectedMask = expectedMask;
        audit.deadlineUs = packet.executeAtMasterUs + packet.ledOnTimeUs + 300000;
        totalSent += sent;
    }

    publishedConnected = connected;
    publishedSynced = synced;
    publishedLeadUs = recommendedLeadUs;
    publishedRttUs = worstRttUs;
    publishedJitterUs = worstJitterUs;
    publishedWorstRssi = worstRssi;
    publishedQuality = synced == 0 ? 3 : systemQuality;
    publishedSent = totalSent;
    publishedAcked = totalAcked;
    publishedMissed = totalMissed;
}

bool wifiSystemReady() {
    return publishedSynced >= MIN_READY_NODES;
}

bool wifiSendTrigger(const TriggerPacket &packet) {
    return triggerQueue && xQueueSend(triggerQueue, &packet, 0) == pdTRUE;
}

uint32_t wifiRecommendedLeadUs() {
    return publishedLeadUs;
}

void wifiMasterPublishUi() {
    const uint8_t connected = publishedConnected;
    const uint8_t synced = publishedSynced;
    uiSetDeviceCounts(connected, synced);
    uiSetSystemReady(synced >= MIN_READY_NODES);
    uiSetNetworkMetrics(publishedSpreadUs, publishedRttUs, publishedJitterUs,
                        publishedLeadUs, publishedHasSpread);
    uiSetReliability(publishedQuality, publishedWorstRssi, publishedSent,
                     publishedAcked, publishedMissed);
}

WifiNodeStatus wifiNodeStatus(uint8_t deviceId) {
    WifiNodeStatus result{};
    ClientRuntime *node = deviceMap[deviceId];
    if (!node || !node->active || !node->client.connected()) return result;
    result.connected = true;
    result.synced = node->synced && node->quality < 3;
    result.rttUs = node->lastRttUs > 0 ? (uint32_t)node->lastRttUs : 0;
    result.jitterUs = node->jitterUs > 0 ? (uint32_t)node->jitterUs : 0;
    result.rssiDbm = node->rssiDbm;
    return result;
}

bool wifiTakeLedAck(WifiLedAck &ack) {
    return ackQueue && xQueueReceive(ackQueue, &ack, 0) == pdTRUE;
}
