#include "ota_service.h"

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <cstring>

#include "config.h"

namespace {
bool serviceEnabled = false;
volatile bool updateInProgress = false;
}

void otaServiceBegin() {
    if (strcmp(OTA_PASSWORD, "CHANGE_ME") == 0 || OTA_PASSWORD[0] == '\0') {
        Serial.println("[OTA] disabled: configure WIFI_AP_PASSWORD/OTA_PASSWORD in secrets.h");
        return;
    }

    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPort(OTA_PORT);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.setRebootOnSuccess(true);
    ArduinoOTA.onStart([]() {
        updateInProgress = true;
        Serial.println("[OTA] update started; triggers and standby paused");
    });
    ArduinoOTA.onEnd([]() {
        Serial.println("[OTA] update complete; rebooting");
    });
    ArduinoOTA.onError([](ota_error_t error) {
        updateInProgress = false;
        Serial.printf("[OTA] update failed: %u\n", (unsigned)error);
    });
    // Progress logging is intentionally omitted. Flash writes already consume
    // resources during OTA; long serial output must not add pressure to Core 0.
    ArduinoOTA.begin();
    serviceEnabled = true;
    Serial.printf("[OTA] ready: %s.local / %s:%u\n", OTA_HOSTNAME,
                  WiFi.softAPIP().toString().c_str(), (unsigned)OTA_PORT);
}

void otaServiceHandle() {
    if (serviceEnabled) ArduinoOTA.handle();
}

bool otaServiceInProgress() {
    return updateInProgress;
}
