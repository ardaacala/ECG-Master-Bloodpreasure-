#pragma once

#define WIFI_SSID "ECG_MASTER"
#define WIFI_PASSWORD "ecgmaster123"
#define MASTER_IP_1 192
#define MASTER_IP_2 168
#define MASTER_IP_3 4
#define MASTER_IP_4 1
#define MASTER_TCP_PORT 3333

#define WIFI_RETRY_INTERVAL_MS 3000
#define TCP_RETRY_INTERVAL_MS 1000
#define SYNC_TIMEOUT_MS 8000
#define EVENT_QUEUE_SIZE 8

#ifndef LED_PIN
#define LED_PIN 4
#endif

#ifndef LED_ACTIVE_LEVEL
#define LED_ACTIVE_LEVEL LOW
#endif

#ifndef ONBOARD_LED_PIN
#define ONBOARD_LED_PIN 2
#endif
