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
#define MIN_READY_NODES 2

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
#define BLE_NODE_NAME "LED_NODE_1"

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
#define UI_FRAME_INTERVAL_MS 16
#define UI_STATUS_INTERVAL_MS 500
#define ENABLE_VERBOSE_TIMING_LOGS 0

#define PIN_NUM_LCD_CS 9
#define PIN_NUM_LCD_PCLK 10
#define PIN_NUM_LCD_DATA0 11
#define PIN_NUM_LCD_DATA1 12
#define PIN_NUM_LCD_DATA2 13
#define PIN_NUM_LCD_DATA3 14
#define PIN_NUM_LCD_RST 21
#define PIN_NUM_TOUCH_SCL 48
#define PIN_NUM_TOUCH_SDA 47
#define DISPLAY_WIDTH 466
#define DISPLAY_HEIGHT 466
#define LCD_BUS_SPEED 80000000
