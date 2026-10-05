#pragma once

// Master bir erisim noktasi acar. Slave kodu daha sonra ayni SSID, parola ve
// TCP portunu kullanacak.
#define WIFI_AP_SSID "ECG_MASTER"
#if __has_include("secrets.h")
#include "secrets.h"
#else
// Documented project default. A local ignored secrets.h may override it.
#define WIFI_AP_PASSWORD "ecgmaster123"
#endif
#define WIFI_TCP_PORT 3333
#define MAX_WIFI_CLIENTS 8
// Tek bir senkron alici sistemi hazir saymak icin yeterlidir. Wi-Fi ve BLE
// state teslimati birbirinden bagimsizdir; biri digerini beklemez.
#define MIN_READY_NODES 1

// OTA runs on the master's existing access point. By default it reuses the
// ignored local AP password; define OTA_PASSWORD separately in secrets.h if
// independent credentials are preferred.
#define OTA_HOSTNAME "bp-trigger-master"
#define OTA_PORT 3232
#ifndef OTA_PASSWORD
#define OTA_PASSWORD WIFI_AP_PASSWORD
#endif
#ifndef BLE_OTA_PASSWORD
#define BLE_OTA_PASSWORD OTA_PASSWORD
#endif

// Karma sistem: bir slave Wi-Fi, GPIO4 active-low slave BLE'dir.
#define WIFI_NODE_ID 1
#define BLE_NODE_ID 2
#define BLE_NODE_NAME "RTCOUND20180"

#define SYSTOLIC 120
#define DIASTOLIC 80
#define BPM 72

#define SYNC_TIMEOUT_MS 8000
#define RESYNC_INTERVAL_MS 3000
#define BLE_SYNC_TIMEOUT_MS 45000
#define BLE_RESYNC_INTERVAL_MS 15000
#define SYNC_SAMPLE_COUNT 1
#define SMALL_PULSE_DURATION_US 40000
#define LARGE_PULSE_DURATION_US 100000
#define ECG_SAMPLE_INTERVAL_MS 16
#define EVENT_QUEUE_SIZE 16
#define ENABLE_VERBOSE_TIMING_LOGS 0

// Ekransiz master: istege bagli yerel nabiz LED'i. Her sentetik nabizda,
// slave'lerin kullandigi ayni planlanmis zamanda 150 ms yanar. -1 kapali.
// PCB'de bos ve strapping olmayan bir GPIO secin (0, 3, 45, 46 olmaz).
#ifndef PULSE_LED_PIN
#define PULSE_LED_PIN -1
#endif
#ifndef PULSE_LED_ACTIVE_LEVEL
#define PULSE_LED_ACTIVE_LEVEL HIGH
#endif
