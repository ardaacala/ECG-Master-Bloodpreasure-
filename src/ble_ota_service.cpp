#include "ble_ota_service.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <cstring>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "config.h"
#include "ota_service.h"
#include "protocol.h"

namespace {
constexpr uint16_t BLE_OTA_MAX_DATA_BYTES = 240;
constexpr uint16_t BLE_OTA_DATA_VALUE_BYTES = BLE_OTA_MAX_DATA_BYTES + 4;
constexpr uint8_t BLE_OTA_QUEUE_LENGTH = 3;
constexpr uint32_t BLE_OTA_MIN_IMAGE_BYTES = 64 * 1024;
constexpr char BLE_OTA_CONFIGURED_PASSWORD[] = BLE_OTA_PASSWORD;
static_assert(sizeof(BLE_OTA_CONFIGURED_PASSWORD) - 1 <= 54,
              "BLE_OTA_PASSWORD must be at most 54 bytes");
static_assert(sizeof(BleOtaStartHeader) == 10,
              "BLE OTA start packet layout changed");
static_assert(sizeof(BleOtaStatus) == 9,
              "BLE OTA status packet layout changed");

enum WorkType : uint8_t {
    WORK_START = 1,
    WORK_DATA = 2,
    WORK_FINISH = 3,
    WORK_ABORT = 4
};

enum ErrorCode : int32_t {
    ERROR_BAD_COMMAND = 1,
    ERROR_AUTH = 2,
    ERROR_BUSY = 3,
    ERROR_QUEUE_FULL = 4,
    ERROR_NO_PARTITION = 5,
    ERROR_IMAGE_SIZE = 6,
    ERROR_OFFSET = 7,
    ERROR_WRITE = 8,
    ERROR_INCOMPLETE = 9,
    ERROR_CRC = 10,
    ERROR_VALIDATE = 11,
    ERROR_BOOT_PARTITION = 12
};

struct OtaWorkItem {
    uint8_t type;
    uint16_t connectionHandle;
    uint32_t valueA;
    uint32_t valueB;
    uint16_t dataLength;
    uint8_t data[BLE_OTA_MAX_DATA_BYTES];
};

QueueHandle_t workQueue = nullptr;
TaskHandle_t otaTaskHandle = nullptr;
NimBLECharacteristic *statusCharacteristic = nullptr;
volatile bool transferInProgress = false;
volatile uint16_t transferConnectionHandle = BLE_HS_CONN_HANDLE_NONE;

void publishStatus(uint8_t code, uint32_t nextOffset, int32_t detail,
                   uint16_t connectionHandle) {
    if (!statusCharacteristic ||
        connectionHandle == BLE_HS_CONN_HANDLE_NONE) return;
    const BleOtaStatus status{code, nextOffset, detail};
    statusCharacteristic->setValue(
        reinterpret_cast<const uint8_t *>(&status), sizeof(status));
    statusCharacteristic->notify(
        reinterpret_cast<const uint8_t *>(&status), sizeof(status),
        connectionHandle);
}

uint32_t updateCrc32(uint32_t crc, const uint8_t *data, size_t length) {
    while (length-- > 0) {
        crc ^= *data++;
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320UL &
                                (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return crc;
}

class OtaControlCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *characteristic,
                 NimBLEConnInfo &connection) override {
        const NimBLEAttValue value = characteristic->getValue();
        if (value.size() < 1 || !workQueue) return;
        const uint8_t *bytes = value.data();
        const uint16_t connectionHandle = connection.getConnHandle();

        if (bytes[0] == BLE_OTA_CMD_START) {
            if (value.size() < sizeof(BleOtaStartHeader)) {
                publishStatus(BLE_OTA_STATUS_ERROR, 0, ERROR_BAD_COMMAND,
                              connectionHandle);
                return;
            }
            BleOtaStartHeader header{};
            memcpy(&header, bytes, sizeof(header));
            const size_t expectedLength = sizeof(header) +
                                          header.passwordLength;
            const size_t configuredLength =
                sizeof(BLE_OTA_CONFIGURED_PASSWORD) - 1;
            if (expectedLength != value.size() ||
                header.passwordLength != configuredLength ||
                memcmp(bytes + sizeof(header), BLE_OTA_CONFIGURED_PASSWORD,
                       configuredLength) != 0) {
                publishStatus(BLE_OTA_STATUS_ERROR, 0, ERROR_AUTH,
                              connectionHandle);
                return;
            }
            if (transferInProgress || otaServiceInProgress()) {
                publishStatus(BLE_OTA_STATUS_ERROR, 0, ERROR_BUSY,
                              connectionHandle);
                return;
            }
            OtaWorkItem item{};
            item.type = WORK_START;
            item.connectionHandle = connectionHandle;
            item.valueA = header.imageSize;
            item.valueB = header.imageCrc32;
            transferConnectionHandle = connectionHandle;
            transferInProgress = true;
            if (xQueueSend(workQueue, &item, 0) != pdTRUE) {
                transferConnectionHandle = BLE_HS_CONN_HANDLE_NONE;
                transferInProgress = false;
                publishStatus(BLE_OTA_STATUS_ERROR, 0, ERROR_QUEUE_FULL,
                              connectionHandle);
            }
            return;
        }

        if (!transferInProgress ||
            transferConnectionHandle != connectionHandle) {
            publishStatus(BLE_OTA_STATUS_ERROR, 0, ERROR_BAD_COMMAND,
                          connectionHandle);
            return;
        }
        OtaWorkItem item{};
        item.connectionHandle = connectionHandle;
        if (bytes[0] == BLE_OTA_CMD_FINISH && value.size() == 1) {
            item.type = WORK_FINISH;
        } else if (bytes[0] == BLE_OTA_CMD_ABORT && value.size() == 1) {
            item.type = WORK_ABORT;
        } else {
            publishStatus(BLE_OTA_STATUS_ERROR, 0, ERROR_BAD_COMMAND,
                          connectionHandle);
            return;
        }
        if (xQueueSend(workQueue, &item, 0) != pdTRUE) {
            publishStatus(BLE_OTA_STATUS_ERROR, 0, ERROR_QUEUE_FULL,
                          connectionHandle);
        }
    }
};

class OtaDataCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *characteristic,
                 NimBLEConnInfo &connection) override {
        const NimBLEAttValue value = characteristic->getValue();
        const uint16_t connectionHandle = connection.getConnHandle();
        if (!workQueue || !transferInProgress ||
            transferConnectionHandle != connectionHandle ||
            value.size() <= 4 || value.size() > BLE_OTA_DATA_VALUE_BYTES) {
            publishStatus(BLE_OTA_STATUS_ERROR, 0, ERROR_BAD_COMMAND,
                          connectionHandle);
            return;
        }
        OtaWorkItem item{};
        item.type = WORK_DATA;
        item.connectionHandle = connectionHandle;
        memcpy(&item.valueA, value.data(), sizeof(item.valueA));
        item.dataLength = value.size() - sizeof(item.valueA);
        memcpy(item.data, value.data() + sizeof(item.valueA), item.dataLength);
        if (xQueueSend(workQueue, &item, 0) != pdTRUE) {
            publishStatus(BLE_OTA_STATUS_ERROR, item.valueA, ERROR_QUEUE_FULL,
                          connectionHandle);
        }
    }
};

OtaControlCallbacks controlCallbacks;
OtaDataCallbacks dataCallbacks;

void otaWorker(void *) {
    esp_ota_handle_t updateHandle = 0;
    const esp_partition_t *updatePartition = nullptr;
    bool handleOpen = false;
    uint32_t expectedSize = 0;
    uint32_t expectedCrc = 0;
    uint32_t written = 0;
    uint32_t runningCrc = 0xFFFFFFFFUL;

    auto resetTransfer = [&]() {
        if (handleOpen) esp_ota_abort(updateHandle);
        updateHandle = 0;
        updatePartition = nullptr;
        handleOpen = false;
        expectedSize = 0;
        expectedCrc = 0;
        written = 0;
        runningCrc = 0xFFFFFFFFUL;
        transferConnectionHandle = BLE_HS_CONN_HANDLE_NONE;
        transferInProgress = false;
    };

    auto failTransfer = [&](uint16_t connectionHandle, int32_t error) {
        const uint32_t failedAt = written;
        resetTransfer();
        publishStatus(BLE_OTA_STATUS_ERROR, failedAt, error,
                      connectionHandle);
        Serial.printf("[BLE OTA] failed at %lu bytes, error=%ld\n",
                      (unsigned long)failedAt, (long)error);
    };

    OtaWorkItem item{};
    for (;;) {
        if (xQueueReceive(workQueue, &item, portMAX_DELAY) != pdTRUE) continue;
        if (item.type == WORK_START) {
            if (handleOpen) {
                failTransfer(item.connectionHandle, ERROR_BUSY);
                continue;
            }
            updatePartition = esp_ota_get_next_update_partition(nullptr);
            if (!updatePartition) {
                failTransfer(item.connectionHandle, ERROR_NO_PARTITION);
                continue;
            }
            expectedSize = item.valueA;
            expectedCrc = item.valueB;
            if (expectedSize < BLE_OTA_MIN_IMAGE_BYTES ||
                expectedSize > updatePartition->size) {
                failTransfer(item.connectionHandle, ERROR_IMAGE_SIZE);
                continue;
            }
            const esp_err_t result = esp_ota_begin(
                updatePartition, expectedSize, &updateHandle);
            if (result != ESP_OK) {
                failTransfer(item.connectionHandle, ERROR_WRITE);
                continue;
            }
            handleOpen = true;
            written = 0;
            runningCrc = 0xFFFFFFFFUL;
            Serial.printf("[BLE OTA] ready for %lu bytes on %s\n",
                          (unsigned long)expectedSize,
                          updatePartition->label);
            publishStatus(BLE_OTA_STATUS_READY, 0,
                          BLE_OTA_MAX_DATA_BYTES, item.connectionHandle);
            continue;
        }

        if (!handleOpen ||
            item.connectionHandle != transferConnectionHandle) continue;
        if (item.type == WORK_ABORT) {
            const uint16_t connectionHandle = item.connectionHandle;
            const uint32_t abortedAt = written;
            resetTransfer();
            publishStatus(BLE_OTA_STATUS_ABORTED, abortedAt, 0,
                          connectionHandle);
            Serial.printf("[BLE OTA] aborted at %lu bytes\n",
                          (unsigned long)abortedAt);
            continue;
        }
        if (item.type == WORK_DATA) {
            if (item.valueA != written ||
                written + item.dataLength > expectedSize) {
                failTransfer(item.connectionHandle, ERROR_OFFSET);
                continue;
            }
            if (esp_ota_write(updateHandle, item.data,
                              item.dataLength) != ESP_OK) {
                failTransfer(item.connectionHandle, ERROR_WRITE);
                continue;
            }
            runningCrc = updateCrc32(runningCrc, item.data,
                                     item.dataLength);
            written += item.dataLength;
            publishStatus(BLE_OTA_STATUS_ACK, written, 0,
                          item.connectionHandle);
            continue;
        }
        if (item.type != WORK_FINISH) continue;
        if (written != expectedSize) {
            failTransfer(item.connectionHandle, ERROR_INCOMPLETE);
            continue;
        }
        const uint32_t finalCrc = runningCrc ^ 0xFFFFFFFFUL;
        if (finalCrc != expectedCrc) {
            failTransfer(item.connectionHandle, ERROR_CRC);
            continue;
        }
        const uint16_t connectionHandle = item.connectionHandle;
        const uint32_t completedSize = written;
        const esp_err_t endResult = esp_ota_end(updateHandle);
        handleOpen = false;
        updateHandle = 0;
        if (endResult != ESP_OK) {
            failTransfer(connectionHandle, ERROR_VALIDATE);
            continue;
        }
        if (esp_ota_set_boot_partition(updatePartition) != ESP_OK) {
            failTransfer(connectionHandle, ERROR_BOOT_PARTITION);
            continue;
        }
        transferConnectionHandle = BLE_HS_CONN_HANDLE_NONE;
        transferInProgress = false;
        publishStatus(BLE_OTA_STATUS_COMPLETE, completedSize,
                      (int32_t)finalCrc, connectionHandle);
        Serial.printf("[BLE OTA] verified %lu bytes; rebooting\n",
                      (unsigned long)completedSize);
        vTaskDelay(pdMS_TO_TICKS(750));
        esp_restart();
    }
}
}  // namespace

bool bleOtaServiceBegin(NimBLEServer *server) {
    if (!server || workQueue) return workQueue != nullptr;
    workQueue = xQueueCreate(BLE_OTA_QUEUE_LENGTH, sizeof(OtaWorkItem));
    if (!workQueue) {
        Serial.println("[BLE OTA] queue allocation failed");
        return false;
    }
    NimBLEService *service = server->createService(BLE_OTA_SERVICE_UUID);
    if (!service) {
        vQueueDelete(workQueue);
        workQueue = nullptr;
        return false;
    }
    NimBLECharacteristic *control = service->createCharacteristic(
        BLE_OTA_CONTROL_UUID, NIMBLE_PROPERTY::WRITE, 64);
    NimBLECharacteristic *data = service->createCharacteristic(
        BLE_OTA_DATA_UUID, NIMBLE_PROPERTY::WRITE,
        BLE_OTA_DATA_VALUE_BYTES);
    statusCharacteristic = service->createCharacteristic(
        BLE_OTA_STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY,
        sizeof(BleOtaStatus));
    auto cleanup = [&]() {
        statusCharacteristic = nullptr;
        server->removeService(service, true);
        vQueueDelete(workQueue);
        workQueue = nullptr;
    };
    if (!control || !data || !statusCharacteristic) {
        cleanup();
        return false;
    }
    control->setCallbacks(&controlCallbacks);
    data->setCallbacks(&dataCallbacks);
    const BleOtaStatus idle{BLE_OTA_STATUS_ABORTED, 0, 0};
    statusCharacteristic->setValue(
        reinterpret_cast<const uint8_t *>(&idle), sizeof(idle));
    if (xTaskCreatePinnedToCore(otaWorker, "ble-ota", 6144, nullptr, 2,
                                &otaTaskHandle, 0) != pdPASS) {
        otaTaskHandle = nullptr;
        cleanup();
        return false;
    }
    Serial.println("[BLE OTA] PC update service ready");
    return true;
}

void bleOtaServiceOnDisconnect(uint16_t connectionHandle) {
    if (!workQueue || !transferInProgress ||
        transferConnectionHandle != connectionHandle) return;
    OtaWorkItem item{};
    item.type = WORK_ABORT;
    item.connectionHandle = connectionHandle;
    xQueueSendToFront(workQueue, &item, 0);
}

bool bleOtaInProgress() {
    return transferInProgress;
}
