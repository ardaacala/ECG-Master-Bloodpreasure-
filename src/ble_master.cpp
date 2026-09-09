#include "ble_master.h"
#include "ble_ota_service.h"
#include "config.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <ctype.h>

static NimBLEClient *client = nullptr;
static NimBLERemoteCharacteristic *rx = nullptr;
static NimBLERemoteCharacteristic *tx = nullptr;
static const NimBLEAdvertisedDevice *pendingDevice = nullptr;
static bool connected = false;
static bool synced = false;
static int64_t offsetUs = 0;
static int64_t bestRttUs = INT64_MAX;
static uint8_t syncSamples = 0;
static int64_t lastRttUs = 0;
static int64_t pendingT1 = 0;
static uint32_t pendingRequest = 0;
static uint32_t nextRequest = 0;
static uint32_t lastSyncRequestMs = 0;
static uint32_t lastSyncMs = 0;
static QueueHandle_t ackQueue = nullptr;
static QueueHandle_t triggerQueue = nullptr;
static QueueHandle_t triggerStateQueue = nullptr;
static QueueHandle_t phoneTriggerQueue = nullptr;
static TaskHandle_t bleTaskHandle = nullptr;
static NimBLECharacteristic *phoneStatus = nullptr;
static volatile bool phoneConnected = false;
static volatile uint16_t phoneBpm = 0;
static volatile uint16_t phoneSystolicMmHg = 0;
static volatile uint16_t phoneDiastolicMmHg = 0;
static volatile bool publishedConnected = false;
static volatile bool publishedSynced = false;
static volatile uint32_t publishedRttUs = 0;
static TriggerStatePacket latestTriggerState{};
static bool latestTriggerStateValid = false;
static uint32_t acknowledgedStateRevision = 0;
static uint32_t pendingStateRevision = 0;
static uint32_t lastStateSendMs = 0;
static uint32_t stateSendIntervalMs = 0;

static constexpr uint8_t BLE_TRIGGER_QUEUE_LENGTH = 16;
static constexpr uint8_t TRIGGER_STATE_PROTOCOL_VERSION = 3;
static constexpr uint32_t TRIGGER_STATE_INITIAL_INTERVAL_MS = 1000;
static constexpr uint32_t TRIGGER_STATE_MAX_INTERVAL_MS = 10000;

static void publishPhoneText(const char *text) {
    if (!phoneStatus || !text) return;
    phoneStatus->setValue(text);
    if (phoneConnected) phoneStatus->notify();
}

class PhoneServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *, NimBLEConnInfo &info) override {
        phoneConnected = true;
        Serial.printf("[BLE PHONE] connected: %s\n",
                      info.getAddress().toString().c_str());
        char status[40];
        snprintf(status, sizeof(status), "BPM %u SYS %u DIA %u",
                 (unsigned)phoneBpm, (unsigned)phoneSystolicMmHg,
                 (unsigned)phoneDiastolicMmHg);
        publishPhoneText(status);
    }

    void onDisconnect(NimBLEServer *, NimBLEConnInfo &info, int reason) override {
        bleOtaServiceOnDisconnect(info.getConnHandle());
        phoneConnected = false;
        Serial.printf("[BLE PHONE] disconnected, reason=%d\n", reason);
        NimBLEDevice::startAdvertising();
    }
};
static PhoneServerCallbacks phoneServerCallbacks;

class PhoneCommandCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *characteristic,
                 NimBLEConnInfo &) override {
        if (bleOtaInProgress()) {
            publishPhoneText("ERR OTA BUSY");
            return;
        }
        std::string value = characteristic->getValue();
        while (!value.empty() && isspace((unsigned char)value.front())) value.erase(0, 1);
        while (!value.empty() && isspace((unsigned char)value.back())) value.pop_back();
        for (char &c : value) c = (char)toupper((unsigned char)c);

        BlePhoneCommand command{BLE_PHONE_TRIGGER, 0, 0, 0, false,
                                PULSE_LARGE, 0};
        bool valid = false;
        const size_t firstComma = value.find(',');
        const size_t secondComma = firstComma == std::string::npos
                                       ? std::string::npos
                                       : value.find(',', firstComma + 1);
        if (firstComma != std::string::npos) {
            const std::string bpmText = value.substr(0, firstComma);
            const std::string sysText = secondComma == std::string::npos
                ? "" : value.substr(firstComma + 1,
                                     secondComma - firstComma - 1);
            const std::string diaText = secondComma == std::string::npos
                ? "" : value.substr(secondComma + 1);
            const bool tripleNumeric = !bpmText.empty() && !sysText.empty() &&
                !diaText.empty() &&
                bpmText.find_first_not_of("0123456789") == std::string::npos &&
                sysText.find_first_not_of("0123456789") == std::string::npos &&
                diaText.find_first_not_of("0123456789") == std::string::npos;
            const long bpm = tripleNumeric ? strtol(bpmText.c_str(), nullptr, 10) : 0;
            const long systolic = tripleNumeric ? strtol(sysText.c_str(), nullptr, 10) : 0;
            const long diastolic = tripleNumeric ? strtol(diaText.c_str(), nullptr, 10) : 0;
            if (!tripleNumeric || bpm < 30 || bpm > 240 ||
                systolic < 31 || systolic > 300 || diastolic < 1 ||
                diastolic >= systolic) {
                publishPhoneText("ERR BPM,SYS,DIA");
                return;
            }
            command.type = BLE_PHONE_SET_BPM;
            command.bpm = (uint16_t)bpm;
            command.systolicMmHg = (uint16_t)systolic;
            command.diastolicMmHg = (uint16_t)diastolic;
            command.hasPressureRange = true;
            valid = true;
        }
        bool numeric = !value.empty();
        for (char c : value) numeric = numeric && isdigit((unsigned char)c);
        if (!valid && numeric) {
            const long bpm = strtol(value.c_str(), nullptr, 10);
            if (bpm < 30 || bpm > 240) {
                publishPhoneText("ERR BPM 30-240");
                return;
            }
            command.type = BLE_PHONE_SET_BPM;
            command.bpm = (uint16_t)bpm;
            valid = true;
        } else if (!valid) {
            if (value == "TRIGGER" || value == "LARGE" || value == "ALL" ||
                value == "LARGE ALL") {
                valid = true;
            } else if (value == "SMALL" || value == "SMALL ALL") {
                command.pulseType = PULSE_SMALL;
                valid = true;
            } else if (value == "WIFI" || value == "LARGE WIFI") {
                command.targetMask = maskForDevice(WIFI_NODE_ID);
                valid = true;
            } else if (value == "SMALL WIFI") {
                command.pulseType = PULSE_SMALL;
                command.targetMask = maskForDevice(WIFI_NODE_ID);
                valid = true;
            } else if (value == "BLE" || value == "LARGE BLE") {
                command.targetMask = maskForDevice(BLE_NODE_ID);
                valid = true;
            } else if (value == "SMALL BLE") {
                command.pulseType = PULSE_SMALL;
                command.targetMask = maskForDevice(BLE_NODE_ID);
                valid = true;
            }
        }

        if (!valid) {
            publishPhoneText("ERR WRITE BPM");
            Serial.printf("[BLE PHONE] invalid command (%u bytes)\n",
                          (unsigned)value.size());
            return;
        }
        if (!phoneTriggerQueue ||
            xQueueSend(phoneTriggerQueue, &command, 0) != pdTRUE) {
            publishPhoneText("ERR BUSY");
            return;
        }
        publishPhoneText(command.type == BLE_PHONE_SET_BPM
                             ? "BPM QUEUED" : "TRIGGER QUEUED");
        Serial.printf("[BLE PHONE] command=%s target=0x%llx\n",
                      value.c_str(), (unsigned long long)command.targetMask);
    }
};
static PhoneCommandCallbacks phoneCommandCallbacks;

static void beginPhoneControlService() {
    NimBLEServer *server = NimBLEDevice::createServer();
    if (!server) {
        Serial.println("[BLE PHONE] server could not be created");
        return;
    }
    server->setCallbacks(&phoneServerCallbacks);
    NimBLEService *service = server->createService(CONTROL_SERVICE_UUID);
    NimBLECharacteristic *command = service->createCharacteristic(
        CONTROL_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    phoneStatus = service->createCharacteristic(
        CONTROL_TX_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    command->setCallbacks(&phoneCommandCallbacks);
    phoneStatus->setValue("WRITE BPM OR BPM,SYS,DIA");
    if (!bleOtaServiceBegin(server)) {
        Serial.println("[BLE OTA] service initialization failed");
    }
    NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
    advertising->setName("BP_TRIGGER_MASTER");
    advertising->addServiceUUID(CONTROL_SERVICE_UUID);
    advertising->enableScanResponse(true);
    advertising->start();
    Serial.println("[BLE PHONE] advertising as BP_TRIGGER_MASTER");
}

class ClientCallbacks : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient *, int reason) override {
        connected = false; synced = false; rx = nullptr; tx = nullptr;
        bestRttUs = INT64_MAX;
        acknowledgedStateRevision = 0;
        pendingStateRevision = 0;
        lastStateSendMs = 0;
        stateSendIntervalMs = 0;
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
    if (data[0] == MSG_TRIGGER_STATE_ACK &&
        length == sizeof(TriggerStateAck)) {
        TriggerStateAck ack{};
        memcpy(&ack, data, sizeof(ack));
        if (ack.protocolVersion == TRIGGER_STATE_PROTOCOL_VERSION &&
            ack.deviceId == BLE_NODE_ID &&
            ack.stateRevision == pendingStateRevision) {
            if (ack.result == TRIGGER_STATE_APPLIED ||
                ack.result == TRIGGER_STATE_UNCHANGED) {
                const bool firstAckForRevision =
                    acknowledgedStateRevision != ack.stateRevision;
                acknowledgedStateRevision = ack.stateRevision;
                if (firstAckForRevision) {
                    Serial.printf("[STATE] BLE device %u revision %lu ACK result=%u\n",
                                  BLE_NODE_ID,
                                  (unsigned long)ack.stateRevision,
                                  ack.result);
                }
            } else {
                Serial.printf("[STATE] BLE device %u revision %lu rejected=%u\n",
                              BLE_NODE_ID, (unsigned long)ack.stateRevision,
                              ack.result);
            }
        }
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
    // BLE request/notification latency is asymmetric. Prefer the lowest RTT
    // sample, which has the smallest possible offset error, and only smooth
    // samples that are close to that best path.
    if (bestRttUs == INT64_MAX || rtt < bestRttUs) {
        offsetUs = measuredOffset;
        bestRttUs = rtt;
    } else if (rtt <= bestRttUs + 5000) {
        offsetUs += (measuredOffset - offsetUs) / 4;
    }
    lastRttUs = rtt;
    SyncCommit commit{MSG_SYNC_COMMIT, response.requestId, offsetUs};
    rx->writeValue((uint8_t *)&commit, sizeof(commit), false);
    lastSyncMs = millis();
    if (syncSamples < 3) ++syncSamples;
    if (!synced && syncSamples >= 3) {
        Serial.printf("[SYNC] BLE %s RTT:%lld OFFSET:%+lld SYNCED\n",
                      BLE_NODE_NAME, (long long)bestRttUs,
                      (long long)offsetUs);
        synced = true;
    }
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
    connected = true; synced = false; bestRttUs = INT64_MAX; syncSamples = 0;
    lastSyncRequestMs = 0;
    acknowledgedStateRevision = 0;
    pendingStateRevision = 0;
    lastStateSendMs = 0;
    stateSendIntervalMs = 0;
    Serial.printf("[BLE] %s connected\n", BLE_NODE_NAME);
}

void bleMasterBegin() {
    ackQueue = xQueueCreate(16, sizeof(BleLedAck));
    triggerQueue = xQueueCreate(BLE_TRIGGER_QUEUE_LENGTH, sizeof(BleTriggerPacket));
    triggerStateQueue = xQueueCreate(1, sizeof(TriggerStatePacket));
    phoneTriggerQueue = xQueueCreate(8, sizeof(BlePhoneCommand));
    NimBLEDevice::init("ECG_MASTER_WIFI_BLE");
    NimBLEDevice::setMTU(247);
    beginPhoneControlService();
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
    // Keep the BLE host available for the OTA GATT server, but suspend slave
    // scanning and sync traffic while firmware chunks are written to flash.
    if (bleOtaInProgress()) return;
    connectPending();
    NimBLEScan *scan = NimBLEDevice::getScan();
    if (!connected && !pendingDevice && !scan->isScanning()) scan->start(5000, false);
    const uint32_t nowMs = millis();
    TriggerStatePacket incomingState{};
    while (triggerStateQueue &&
           xQueueReceive(triggerStateQueue, &incomingState, 0) == pdTRUE) {
        latestTriggerState = incomingState;
        latestTriggerStateValid = true;
    }
    if (synced && nowMs - lastSyncMs >= BLE_SYNC_TIMEOUT_MS) {
        synced = false; bestRttUs = INT64_MAX; syncSamples = 0;
        Serial.println("[SYNC] BLE timeout");
    }
    const uint32_t syncIntervalMs = synced ? BLE_RESYNC_INTERVAL_MS : 500;
    if (connected && rx && nowMs - lastSyncRequestMs >= syncIntervalMs) {
        pendingT1 = esp_timer_get_time();
        pendingRequest = ++nextRequest;
        lastSyncRequestMs = nowMs;
        SyncRequest request{MSG_SYNC_REQ, pendingRequest, pendingT1};
        rx->writeValue((uint8_t *)&request, sizeof(request), false);
    }

    const bool isReceiver = latestTriggerStateValid &&
        (latestTriggerState.receiverDeviceId == 0 ||
         latestTriggerState.receiverDeviceId == BLE_NODE_ID);
    if (connected && synced && rx && isReceiver) {
        if (pendingStateRevision != latestTriggerState.stateRevision) {
            pendingStateRevision = latestTriggerState.stateRevision;
            lastStateSendMs = 0;
            stateSendIntervalMs = TRIGGER_STATE_INITIAL_INTERVAL_MS;
        }
        if (lastStateSendMs == 0 ||
            nowMs - lastStateSendMs >= stateSendIntervalMs) {
            const bool immediateSend = lastStateSendMs == 0;
            lastStateSendMs = nowMs;
            if (!rx->writeValue((uint8_t *)&latestTriggerState,
                                sizeof(latestTriggerState), false)) {
                Serial.printf("[STATE] BLE revision %lu send failed\n",
                              (unsigned long)latestTriggerState.stateRevision);
            }
            if (!immediateSend) {
                stateSendIntervalMs = min<uint32_t>(
                    stateSendIntervalMs * 2,
                    TRIGGER_STATE_MAX_INTERVAL_MS);
            }
        }
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

bool blePublishTriggerState(const TriggerStatePacket &packet) {
    return triggerStateQueue &&
           xQueueOverwrite(triggerStateQueue, &packet) == pdTRUE;
}

bool bleTakeLedAck(BleLedAck &ack) {
    return ackQueue && xQueueReceive(ackQueue, &ack, 0) == pdTRUE;
}

bool bleTakePhoneCommand(BlePhoneCommand &command) {
    return phoneTriggerQueue &&
           xQueueReceive(phoneTriggerQueue, &command, 0) == pdTRUE;
}

void blePublishBpm(uint16_t bpm, uint16_t systolicMmHg,
                   uint16_t diastolicMmHg) {
    phoneBpm = bpm;
    phoneSystolicMmHg = systolicMmHg;
    phoneDiastolicMmHg = diastolicMmHg;
    char result[48];
    snprintf(result, sizeof(result), "BPM %u SYS %u DIA %u SAVED",
             (unsigned)bpm, (unsigned)systolicMmHg,
             (unsigned)diastolicMmHg);
    publishPhoneText(result);
}

void blePublishPhoneResult(uint32_t eventId, bool wifiQueued, bool bleQueued) {
    char result[48];
    if (!wifiQueued && !bleQueued) {
        snprintf(result, sizeof(result), "OK %lu LOCAL=1",
                 (unsigned long)eventId);
    } else {
        snprintf(result, sizeof(result), "OK %lu WIFI=%u BLE=%u",
                 (unsigned long)eventId, wifiQueued ? 1U : 0U,
                 bleQueued ? 1U : 0U);
    }
    publishPhoneText(result);
}

uint32_t bleRecommendedLeadUs() {
    const uint32_t rttUs = publishedRttUs;
    if (!publishedSynced || rttUs == 0) return 200000;
    return min<uint32_t>(600000, 100000 + 2 * rttUs);
}
