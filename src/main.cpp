/* Live cuff-pressure gauge for a conditioned 0-40 kPa analog sensor.
 * Sensor output: nominally 0.2-2.7 V at a 3.3 V supply. */
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include <Preferences.h>
#include "config.h"
#include "protocol.h"
#include "wifi_master.h"
#include "ble_master.h"
#include "ota_service.h"

// The active 466x466 round Waveshare board routes its onboard BAT_ADC divider
// to GPIO4. The external conditioned pressure sensor remains on GPIO2.
constexpr uint8_t BATTERY_ADC_PIN = 4;
constexpr uint8_t PRESSURE_ADC_PIN = 2;
// Waveshare's official ADC example for this 1.43-inch board reads ADC1
// channel 3 (GPIO4) and multiplies the ADC-side voltage by 3.
constexpr float BATTERY_DIVIDER_RATIO = 3.0f;
constexpr uint32_t BATTERY_UPDATE_MS = 5000;
constexpr uint8_t BATTERY_SAMPLE_COUNT = 32;
constexpr uint16_t BATTERY_SAMPLE_SPACING_MS = 20;
// The schematic fits 82 kOhm at ETA6098 ISET, which programs 0.5 A. While
// charging, BAT_ADC is a terminal/charger voltage and is not a usable OCV SOC
// reading. Detect its sustained connection step, retain the pre-charge SOC,
// then integrate a 1500 mAh CC/CV model instead of mapping 4.2 V to 100%.
constexpr float BATTERY_CAPACITY_MAH = 1500.0f;
constexpr float BATTERY_CHARGE_CURRENT_MA = 500.0f;
// ISET specifies battery-side current. This is the cell's approximate
// coulombic acceptance factor, not the charger's power-conversion efficiency.
constexpr float BATTERY_CHARGE_COULOMBIC_EFFICIENCY = 0.98f;
constexpr uint16_t BATTERY_CHARGE_STEP_MV = 50;
constexpr uint16_t BATTERY_CHARGE_HOLD_MV = 35;
constexpr uint8_t BATTERY_CHARGE_CONFIRM_SAMPLES = 3;
constexpr uint16_t BATTERY_FAST_DROP_MV = 70;
constexpr uint16_t BATTERY_FULL_UNPLUG_MV = 4160;
constexpr float BATTERY_VOLTAGE_RISE_ALPHA = 0.08f;
constexpr float BATTERY_VOLTAGE_FALL_ALPHA = 0.35f;
constexpr float BATTERY_SOC_FALL_ALPHA = 0.45f;
constexpr float BATTERY_SOC_RISE_ALPHA = 0.12f;
constexpr float BATTERY_SOC_HYSTERESIS_PERCENT = 1.0f;
constexpr uint16_t ADC_RESOLUTION_MAX = 4095;
constexpr uint16_t SENSOR_NOMINAL_ZERO_MV = 200;
constexpr uint16_t SENSOR_ZERO_MIN_VALID_MV = 100;
constexpr uint16_t SENSOR_ZERO_MAX_VALID_MV = 450;
// Change only this value if the measured full-scale output is not 2.700 V.
constexpr uint16_t SENSOR_FULL_SCALE_MV = 2700;
constexpr float SENSOR_FULL_SCALE_KPA = 40.0f;
constexpr float KPA_TO_MMHG = 7.50061683f;
constexpr float SENSOR_FULL_SCALE_MMHG =
    SENSOR_FULL_SCALE_KPA * KPA_TO_MMHG;
// Reference comparison showed roughly 10 mmHg under-reading around 50 mmHg
// and 20 mmHg around 100 mmHg. A gain correction preserves the zero point and
// avoids the discontinuity that a +10/+20 piecewise offset would create.
constexpr float PRESSURE_GAIN_CORRECTION = 1.20f;
constexpr float GAUGE_MAX_MMHG = 300.0f;
constexpr uint16_t ADC_SAMPLE_HZ = 500;
constexpr uint32_t ADC_SAMPLE_PERIOD_US = 1000000UL / ADC_SAMPLE_HZ;
constexpr uint8_t PRESSURE_MEDIAN_WINDOW = 5;
constexpr float PRESSURE_SPIKE_STEP_MMHG = 2.5f;
constexpr float PRESSURE_SPIKE_RETURN_MMHG = 1.0f;
constexpr uint8_t PRESSURE_SPIKE_CONFIRM_SAMPLES = 8;
constexpr uint16_t UI_REFRESH_HZ = 30;
constexpr uint32_t UI_FRAME_PERIOD_US = 1000000UL / UI_REFRESH_HZ;
constexpr uint16_t PULSE_LED_ON_MS = 150;
// Ignore brief ADC excursions around the configured pressure boundaries.
// The beat timeline keeps running during this window, so boundary noise
// cannot turn 30 BPM into rapid restarts or make 200 BPM appear very slow.
constexpr uint16_t FAKE_RANGE_EXIT_DEBOUNCE_MS = 300;
// The measured cuff oscillations are typically only a few tenths of a mmHg.
// Keep the simulator in that range instead of drawing an exaggerated 2 mmHg
// pressure excursion.
constexpr float FAKE_PULSE_AMPLITUDE_MMHG = 1.5f;
constexpr uint16_t DEFAULT_FAKE_BPM = 72;
constexpr uint16_t DEFAULT_FAKE_MAX_PRESSURE_MMHG = 130;
constexpr uint16_t DEFAULT_FAKE_MIN_PRESSURE_MMHG = 30;
// Shared by real, synthetic and phone-triggered beats. 220 ms still permits
// the configured maximum of 240 BPM (250 ms period) while rejecting a second
// event created during a real/synthetic handover or repeated BLE write.
// Global transport/phone de-duplication. 200 ms permits 240 BPM (250 ms
// period), while the real cuff detector retains its separate 500 ms lock.
constexpr uint32_t MIN_TRIGGER_INTERVAL_MS = 200;
// A changing transport lead shifts executeAt between beats and can make a
// valid beat collide with the global duplicate guard. Use one conservative
// lead for every event so the physical/UI cadence remains exactly BPM-based.
constexpr uint32_t FIXED_TRIGGER_LEAD_US = 600000;
// A cuff oscillation can contain a reflected/dicrotic second positive lobe.
// Keep the real detector refractory long enough not to count that lobe as a
// second heartbeat. 500 ms still supports heart rates up to 120 BPM.
constexpr uint16_t WARMUP_SAMPLE_COUNT = 125;
constexpr uint16_t ZERO_SAMPLE_COUNT = 250;
constexpr uint8_t LCD_CS = 9, LCD_PCLK = 10, LCD_D0 = 11, LCD_D1 = 12;
constexpr uint8_t LCD_D2 = 13, LCD_D3 = 14, LCD_RST = 21;
// FT3168/FT6146 touch controller and QMI8658 share this onboard I2C bus.
// Both are accessed from Arduino loop/Core 0 so their transactions cannot
// collide with the priority UI task on Core 1.
constexpr uint8_t TOUCH_SDA_PIN = 47, TOUCH_SCL_PIN = 48;
constexpr uint8_t TOUCH_I2C_ADDRESS = 0x38;
constexpr uint16_t TOUCH_POLL_INTERVAL_MS = 25;
constexpr uint16_t TOUCH_RETRY_INTERVAL_MS = 2000;
constexpr uint8_t TOUCH_FAILURE_LIMIT = 3;
constexpr int16_t SWIPE_MIN_DISTANCE = 70;
constexpr uint16_t SWIPE_MAX_DURATION_MS = 1000;
constexpr uint16_t SCREEN_HOLD_TOGGLE_MS = 2000;
// QMI8658 INT1 is routed to GPIO8 on this exact board. Wake-on-Motion uses
// the accelerometer's low-power engine; the gyroscope must stay disabled for
// that mode according to the sensor data sheet.
constexpr uint8_t IMU_INT_PIN = 8;
constexpr uint8_t QMI8658_ADDRESS_HIGH = 0x6B;
constexpr uint8_t QMI8658_ADDRESS_LOW = 0x6A;
constexpr uint8_t QMI8658_WHO_AM_I = 0x05;
constexpr uint8_t QMI8658_REG_WHO_AM_I = 0x00;
constexpr uint8_t QMI8658_REG_CTRL1 = 0x02;
constexpr uint8_t QMI8658_REG_CTRL2 = 0x03;
constexpr uint8_t QMI8658_REG_CTRL7 = 0x08;
constexpr uint8_t QMI8658_REG_CTRL9 = 0x0A;
constexpr uint8_t QMI8658_REG_CAL1_L = 0x0B;
constexpr uint8_t QMI8658_REG_CAL1_H = 0x0C;
constexpr uint8_t QMI8658_REG_STATUS_INT = 0x2D;
constexpr uint8_t QMI8658_REG_STATUS1 = 0x2F;
constexpr uint8_t QMI8658_REG_AX_L = 0x35;
constexpr uint8_t QMI8658_STATUS1_CMD_DONE = 0x01;
constexpr uint8_t QMI8658_STATUS1_WOM = 0x04;
constexpr uint8_t QMI8658_STATUS_INT_CMD_DONE = 0x80;
constexpr uint8_t QMI8658_CTRL9_NOP = 0x00;
constexpr uint8_t QMI8658_CTRL9_WRITE_WOM = 0x08;
constexpr uint8_t QMI8658_CTRL1_INT1_PUSH_PULL = 0x08;
constexpr uint8_t QMI8658_CTRL1_AUTO_INCREMENT = 0x40;
constexpr uint8_t QMI8658_ACCEL_21HZ_LOW_POWER = 0x0D;
constexpr uint8_t QMI8658_WOM_THRESHOLD_MG = 50;
constexpr uint8_t QMI8658_WOM_BLANKING_SAMPLES = 8;
constexpr uint16_t QMI8658_WOM_REARM_DELAY_MS = 450;
constexpr uint16_t IMU_ACTIVITY_SAMPLE_MS = 100;
// +/-2 g gives 16384 LSB/g. About 11 mg catches slow hand movement while
// remaining above the stationary low-power accelerometer noise floor.
constexpr int32_t IMU_ACTIVITY_DELTA_LSB = 180;
constexpr uint32_t STANDBY_TIMEOUT_MS = 5UL * 60UL * 1000UL;
constexpr uint32_t STANDBY_SETTINGS_MAGIC = 0x42505332;
constexpr int16_t SCREEN_W = 466, SCREEN_H = 466;

struct StandbyRetainedSettings {
    uint32_t magic;
    uint16_t bpm;
    uint16_t systolicMmHg;
    uint16_t diastolicMmHg;
    uint8_t darkTheme;
};

RTC_DATA_ATTR StandbyRetainedSettings standbyRetainedSettings = {};

Arduino_DataBus *bus = nullptr;
Arduino_GFX *display = nullptr;
Arduino_Canvas *canvas = nullptr;
uint16_t *backgroundPixels = nullptr;
uint16_t *dirtyPixels = nullptr;
int64_t zeroRawSum = 0;
int64_t zeroMilliVoltsSum = 0;
int32_t zeroRaw = 0;
uint32_t zeroMilliVolts = SENSOR_NOMINAL_ZERO_MV;
uint16_t warmupSamples = 0;
volatile uint16_t zeroSamples = 0;
volatile int32_t latestRaw = 0;
volatile uint32_t latestMilliVolts = 0;
volatile bool adcSaturated = false;
volatile float targetPressure = 0.0f;
volatile float displayedGaugeBasePressure = 0.0f;
volatile float fakePressureOffset = 0.0f;
volatile uint16_t fakeBpm = DEFAULT_FAKE_BPM;
volatile uint16_t fakeMaxPressureMmHg = DEFAULT_FAKE_MAX_PRESSURE_MMHG;
volatile uint16_t fakeMinPressureMmHg = DEFAULT_FAKE_MIN_PRESSURE_MMHG;
volatile uint32_t fakeBpmRevision = 0;
// Incremented whenever a pressure boundary invalidates an already queued or
// visible synthetic event. The UI consumes the event instead of showing it
// later after the pressure/range changes.
volatile uint32_t fakeUiCancelRevision = 0;
volatile bool fakePulseMode = false;
volatile uint32_t visiblePulseCount = 0;
volatile uint16_t measuredSps = 0;
volatile uint16_t measuredUiFps = 0;
volatile uint32_t lastUiFrameMs = 0;
volatile uint16_t maxUiDrawMs = 0;
volatile uint16_t batteryVoltageMv = 0;
volatile uint8_t batteryPercent = 0;
volatile bool batteryReadingValid = false;
volatile bool batteryChargingEstimated = false;
volatile bool darkTheme = false;
volatile uint32_t themeRevision = 0;
volatile bool requestedScreenOn = true;
volatile uint32_t screenPowerRevision = 0;
bool touchAvailable = false;
bool imuMotionWakeAvailable = false;
uint8_t qmi8658Address = 0;
uint32_t lastMotionActivityMs = 0;
volatile bool standbyEntering = false;
uint32_t lastSerialMs = 0;
float lastNeedlePressure = -1.0f;
uint32_t networkEventId = 0;
int64_t lastTriggerExecuteAtUs = 0;
portMUX_TYPE triggerMux = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t uiTaskHandle = nullptr;

uint8_t batteryPercentFromMillivolts(uint16_t millivolts) {
    struct BatteryPoint { uint16_t mv; uint8_t percent; };
    static constexpr BatteryPoint curve[] = {
        {3200, 0}, {3300, 5}, {3500, 10}, {3600, 20}, {3700, 35},
        {3800, 50}, {3900, 65}, {4000, 80}, {4100, 90}, {4200, 100}
    };
    if (millivolts <= curve[0].mv) return 0;
    constexpr size_t count = sizeof(curve) / sizeof(curve[0]);
    if (millivolts >= curve[count - 1].mv) return 100;
    for (size_t i = 1; i < count; ++i) {
        if (millivolts <= curve[i].mv) {
            const uint16_t spanMv = curve[i].mv - curve[i - 1].mv;
            const uint16_t offsetMv = millivolts - curve[i - 1].mv;
            const uint8_t spanPercent = curve[i].percent - curve[i - 1].percent;
            return constrain((int)curve[i - 1].percent +
                                 (int)offsetMv * spanPercent / spanMv,
                             0, 100);
        }
    }
    return 0;
}

float batteryChargeTaperFactor(float percent) {
    // ETA6098 charges at constant current for most of the cycle and tapers near
    // 4.2 V. These bands keep the time estimate realistic near full charge.
    if (percent < 85.0f) return 1.0f;
    if (percent < 93.0f) return 0.72f;
    if (percent < 98.0f) return 0.42f;
    return 0.16f;
}

float advanceBatteryChargeEstimate(float percent, uint32_t elapsedMs) {
    const float idealPercentPerMs =
        (BATTERY_CHARGE_CURRENT_MA *
         BATTERY_CHARGE_COULOMBIC_EFFICIENCY * 100.0f) /
        (BATTERY_CAPACITY_MAH * 3600000.0f);
    return min(100.0f,
               percent + idealPercentPerMs * (float)elapsedMs *
                             batteryChargeTaperFactor(percent));
}

void updateBatteryMeasurement() {
    // Spread the 32 ADC conversions across ~640 ms. A former burst loop did
    // all conversions at once every five seconds and briefly stalled the
    // pressure stream, which looked like a periodic gauge/UI freeze.
    static uint32_t adcMillivoltSum = 0;
    static uint8_t adcSampleCount = 0;
    static uint32_t nextBatchMs = 0;
    static uint32_t nextSampleMs = 0;
    const uint32_t nowMs = millis();

    if (adcSampleCount == 0) {
        if (nextBatchMs != 0 && (int32_t)(nowMs - nextBatchMs) < 0) return;
        adcMillivoltSum = 0;
        nextSampleMs = nowMs;
    }
    if ((int32_t)(nowMs - nextSampleMs) < 0) return;

    adcMillivoltSum += analogReadMilliVolts(BATTERY_ADC_PIN);
    ++adcSampleCount;
    nextSampleMs = nowMs + BATTERY_SAMPLE_SPACING_MS;
    if (adcSampleCount < BATTERY_SAMPLE_COUNT) return;

    const float measuredBatteryMv =
        ((float)adcMillivoltSum / BATTERY_SAMPLE_COUNT) *
        BATTERY_DIVIDER_RATIO;
    adcSampleCount = 0;
    nextBatchMs = nowMs + BATTERY_UPDATE_MS;

    static float filteredBatteryMv = 0.0f;
    static float estimatedBatteryPercent = -1.0f;
    static uint16_t previousMeasuredBatteryMv = 0;
    static uint32_t previousUpdateMs = 0;
    static bool chargeCandidate = false;
    static uint8_t chargeConfirmSamples = 0;
    static uint16_t preChargeBatteryMv = 0;
    static float preChargeBatteryPercent = 0.0f;
    static uint32_t chargeCandidateStartedMs = 0;
    static bool charging = false;
    static uint16_t chargingPeakMv = 0;
    static uint8_t fullUnplugConfirmSamples = 0;
    const uint16_t measuredMv = (uint16_t)constrain(
        lroundf(measuredBatteryMv), 0L, 5000L);
    const bool fastVoltageDrop =
        previousMeasuredBatteryMv >= 2500 &&
        previousMeasuredBatteryMv > measuredMv &&
        previousMeasuredBatteryMv - measuredMv >= BATTERY_FAST_DROP_MV;
    const bool dropFromChargingPeak =
        charging && chargingPeakMv > measuredMv &&
        chargingPeakMv - measuredMv >= BATTERY_FAST_DROP_MV;
    if (charging && estimatedBatteryPercent >= 98.0f &&
        measuredMv < BATTERY_FULL_UNPLUG_MV) {
        if (fullUnplugConfirmSamples < UINT8_MAX) {
            ++fullUnplugConfirmSamples;
        }
    } else {
        fullUnplugConfirmSamples = 0;
    }
    const bool fullChargeCableRemoved =
        fullUnplugConfirmSamples >= BATTERY_CHARGE_CONFIRM_SAMPLES;
    const bool chargerRemoved = fastVoltageDrop || dropFromChargingPeak ||
                                fullChargeCableRemoved;

    if (filteredBatteryMv == 0.0f) {
        filteredBatteryMv = measuredBatteryMv;
    } else {
        const float alpha = chargerRemoved
                                ? 1.0f
                                : (measuredBatteryMv < filteredBatteryMv
                                       ? BATTERY_VOLTAGE_FALL_ALPHA
                                       : BATTERY_VOLTAGE_RISE_ALPHA);
        filteredBatteryMv += alpha * (measuredBatteryMv - filteredBatteryMv);
    }
    uint16_t publishedMv = (uint16_t)constrain(
        lroundf(filteredBatteryMv), 0L, 5000L);
    float voltagePercent =
        (float)batteryPercentFromMillivolts(publishedMv);
    const uint32_t elapsedMs = previousUpdateMs == 0
                                   ? BATTERY_UPDATE_MS
                                   : nowMs - previousUpdateMs;

    if (estimatedBatteryPercent < 0.0f) {
        estimatedBatteryPercent = voltagePercent;
        batteryChargingEstimated = false;
    } else if (chargerRemoved) {
        // USB removal normally causes a clear downward terminal-voltage step.
        // Snap to the battery-only OCV estimate and leave charging mode.
        filteredBatteryMv = measuredBatteryMv;
        publishedMv = measuredMv;
        voltagePercent = (float)batteryPercentFromMillivolts(publishedMv);
        estimatedBatteryPercent = voltagePercent;
        charging = false;
        chargeCandidate = false;
        chargeConfirmSamples = 0;
        fullUnplugConfirmSamples = 0;
        batteryChargingEstimated = false;
    } else if (charging) {
        chargingPeakMv = max(chargingPeakMv, measuredMv);
        estimatedBatteryPercent = advanceBatteryChargeEstimate(
            estimatedBatteryPercent, elapsedMs);
        batteryChargingEstimated = true;
    } else {
        const bool newChargeStep =
            previousMeasuredBatteryMv >= 2500 &&
            measuredMv >= previousMeasuredBatteryMv + BATTERY_CHARGE_STEP_MV;

        if (!chargeCandidate && newChargeStep) {
            chargeCandidate = true;
            chargeConfirmSamples = 1;
            preChargeBatteryMv = previousMeasuredBatteryMv;
            preChargeBatteryPercent = estimatedBatteryPercent;
            chargeCandidateStartedMs = nowMs;
        } else if (chargeCandidate) {
            if (measuredMv >= preChargeBatteryMv + BATTERY_CHARGE_HOLD_MV) {
                if (chargeConfirmSamples < UINT8_MAX) ++chargeConfirmSamples;
            } else {
                chargeCandidate = false;
                chargeConfirmSamples = 0;
            }
        }

        if (chargeCandidate &&
            chargeConfirmSamples >= BATTERY_CHARGE_CONFIRM_SAMPLES) {
            charging = true;
            chargingPeakMv = measuredMv;
            chargeCandidate = false;
            chargeConfirmSamples = 0;
            // Include the short confirmation interval without ever using the
            // charge-biased voltage as a percentage.
            estimatedBatteryPercent = advanceBatteryChargeEstimate(
                preChargeBatteryPercent, nowMs - chargeCandidateStartedMs);
            batteryChargingEstimated = true;
        } else if (chargeCandidate) {
            // Hold the last battery-only result during the 15 s confirmation.
            estimatedBatteryPercent = preChargeBatteryPercent;
            batteryChargingEstimated = false;
        } else if (voltagePercent >
                   estimatedBatteryPercent + BATTERY_SOC_HYSTERESIS_PERCENT) {
            estimatedBatteryPercent +=
                BATTERY_SOC_RISE_ALPHA *
                (voltagePercent - estimatedBatteryPercent);
            batteryChargingEstimated = false;
        } else if (voltagePercent + BATTERY_SOC_HYSTERESIS_PERCENT <
                   estimatedBatteryPercent) {
            estimatedBatteryPercent +=
                BATTERY_SOC_FALL_ALPHA *
                (voltagePercent - estimatedBatteryPercent);
            batteryChargingEstimated = false;
        } else {
            batteryChargingEstimated = false;
        }
    }

    batteryVoltageMv = publishedMv;
    batteryPercent = (uint8_t)constrain(
        lroundf(estimatedBatteryPercent), 0L, 100L);
    // A disconnected divider is near zero. USB/charging may make the voltage
    // rise; percentage remains clamped by the lookup-table endpoints.
    batteryReadingValid = publishedMv >= 2500;
    previousMeasuredBatteryMv = measuredMv;
    previousUpdateMs = nowMs;
}

bool sendPulseTrigger(PulseType pulseType = PULSE_LARGE,
                      uint64_t targetMask = 0,
                      int64_t *executeAtResultUs = nullptr,
                      uint32_t *eventIdResult = nullptr) {
    if (standbyEntering || otaServiceInProgress()) return false;
    const int64_t executeAtUs = esp_timer_get_time() +
                                FIXED_TRIGGER_LEAD_US;
    uint32_t eventId = 0;
    bool accepted = false;
    portENTER_CRITICAL(&triggerMux);
    if (lastTriggerExecuteAtUs == 0 ||
        executeAtUs - lastTriggerExecuteAtUs >=
            (int64_t)MIN_TRIGGER_INTERVAL_MS * 1000) {
        lastTriggerExecuteAtUs = executeAtUs;
        eventId = ++networkEventId;
        accepted = true;
    }
    portEXIT_CRITICAL(&triggerMux);
    if (!accepted) {
        Serial.println("[PULSE TRIGGER] duplicate suppressed");
        return false;
    }

    if (executeAtResultUs) *executeAtResultUs = executeAtUs;
    if (eventIdResult) *eventIdResult = eventId;
    const uint32_t durationUs = pulseType == PULSE_SMALL
                                    ? SMALL_PULSE_DURATION_US
                                    : LARGE_PULSE_DURATION_US;
    const TriggerPacket packet{MSG_TRIGGER, eventId, pulseType,
                               targetMask, executeAtUs, durationUs};
    const bool wifiQueued = wifiSendTrigger(packet);
    const bool bleQueued = bleSendTrigger(packet);
    Serial.printf("[PULSE TRIGGER] #%lu wifi=%u ble=%u at=%lld\n",
                  (unsigned long)eventId, wifiQueued ? 1U : 0U,
                  bleQueued ? 1U : 0U, (long long)executeAtUs);
    blePublishPhoneResult(packet.eventId, wifiQueued, bleQueued);
    return true;
}

constexpr int16_t GAUGE_CX = SCREEN_W / 2;
constexpr int16_t GAUGE_CY = SCREEN_H / 2;
constexpr int16_t NEEDLE_RADIUS = 158;
constexpr int16_t PULSE_LED_CX = GAUGE_CX;
constexpr int16_t PULSE_LED_CY = 414;
constexpr int16_t PULSE_LED_RADIUS = 16;

void pointOnGauge(float pressure, int16_t radius, int16_t &x, int16_t &y) {
    pressure = constrain(pressure, 0.0f, GAUGE_MAX_MMHG);
    const float angle = (120.0f + pressure) * DEG_TO_RAD;
    x = GAUGE_CX + (int16_t)lroundf(cosf(angle) * radius);
    y = GAUGE_CY + (int16_t)lroundf(sinf(angle) * radius);
}

void drawThickLine(Arduino_GFX *gfx, int16_t x0, int16_t y0,
                   int16_t x1, int16_t y1, uint16_t color, uint8_t width) {
    const float dx = x1 - x0, dy = y1 - y0;
    const float length = sqrtf(dx * dx + dy * dy);
    if (length < 1.0f) return;
    const float ox = -dy / length, oy = dx / length;
    const int half = width / 2;
    for (int i = -half; i <= half; ++i) {
        gfx->drawLine(x0 + lroundf(ox * i), y0 + lroundf(oy * i),
                      x1 + lroundf(ox * i), y1 + lroundf(oy * i), color);
    }
}

void drawPulseLed(Arduino_GFX *gfx, bool on) {
    const uint16_t bezel = gfx->color565(24, 24, 24);
    const uint16_t rim = gfx->color565(92, 92, 92);
    const uint16_t offColor = gfx->color565(58, 8, 10);
    const uint16_t onColor = gfx->color565(255, 24, 36);
    const uint16_t highlight = gfx->color565(255, 178, 178);

    gfx->fillCircle(PULSE_LED_CX, PULSE_LED_CY, PULSE_LED_RADIUS + 4, bezel);
    gfx->fillCircle(PULSE_LED_CX, PULSE_LED_CY, PULSE_LED_RADIUS + 1, rim);
    gfx->fillCircle(PULSE_LED_CX, PULSE_LED_CY, PULSE_LED_RADIUS,
                    on ? onColor : offColor);
    if (on) {
        gfx->fillCircle(PULSE_LED_CX - 5, PULSE_LED_CY - 5, 4, highlight);
    }
}

void drawStaticGauge() {
    const bool dark = darkTheme;
    const uint16_t outer = canvas->color565(dark ? 0 : 18,
                                             dark ? 0 : 18,
                                             dark ? 0 : 18);
    const uint16_t face = canvas->color565(dark ? 5 : 244,
                                            dark ? 7 : 240,
                                            dark ? 9 : 230);
    const uint16_t ink = canvas->color565(dark ? 225 : 18,
                                           dark ? 228 : 18,
                                           dark ? 232 : 18);
    canvas->fillScreen(outer);
    canvas->fillCircle(GAUGE_CX, GAUGE_CY, 225, face);
    for (int r = 218; r <= 225; ++r) canvas->drawCircle(GAUGE_CX, GAUGE_CY, r, ink);

    for (int i = 0; i <= 60; ++i) {
        const float pressure = i * 5.0f;
        const bool major = (i % 4) == 0;
        int16_t xo, yo, xi, yi;
        pointOnGauge(pressure, 207, xo, yo);
        pointOnGauge(pressure, major ? 177 : 190, xi, yi);
        drawThickLine(canvas, xi, yi, xo, yo, ink, major ? 5 : 2);

        if (major) {
            int16_t tx, ty, x1, y1;
            uint16_t tw, th;
            pointOnGauge(pressure, 151, tx, ty);
            char label[8];
            snprintf(label, sizeof(label), "%d", (int)pressure);
            canvas->setTextSize(2);
            canvas->setTextColor(ink);
            canvas->getTextBounds(label, 0, 0, &x1, &y1, &tw, &th);
            canvas->setCursor(tx - tw / 2, ty - th / 2);
            canvas->print(label);
        }
    }
    drawPulseLed(canvas, false);
}

bool readTouchPoint(int16_t &x, int16_t &y, bool &pressed) {
    if (!touchAvailable) return false;
    Wire.beginTransmission(TOUCH_I2C_ADDRESS);
    Wire.write((uint8_t)0x02); // number of points, then first point coordinates
    // Both onboard I2C devices retain their register pointer across STOP.
    // Avoid the Arduino I2C driver's sporadic repeated-start INVALID_STATE.
    if (Wire.endTransmission(true) != 0) return false;
    if (Wire.requestFrom(TOUCH_I2C_ADDRESS, (uint8_t)5) != 5) return false;
    const uint8_t points = Wire.read() & 0x0f;
    const uint8_t xh = Wire.read();
    const uint8_t xl = Wire.read();
    const uint8_t yh = Wire.read();
    const uint8_t yl = Wire.read();
    pressed = points > 0;
    if (pressed) {
        x = ((xh & 0x0f) << 8) | xl;
        y = ((yh & 0x0f) << 8) | yl;
    }
    return true;
}

bool initializeTouchBus(bool restartBus) {
    if (restartBus) Wire.end();
    if (!Wire.begin(TOUCH_SDA_PIN, TOUCH_SCL_PIN, 400000)) return false;
    Wire.setTimeOut(10);
    Wire.beginTransmission(TOUCH_I2C_ADDRESS);
    return Wire.endTransmission() == 0;
}

bool qmi8658ReadRegisterAt(uint8_t address, uint8_t reg, uint8_t &value) {
    Wire.beginTransmission(address);
    Wire.write(reg);
    // QMI8658 retains the register pointer across STOP. Avoiding a repeated
    // start here keeps the shared Arduino I2C peripheral out of the sporadic
    // INVALID_STATE seen when touch and IMU no-stop reads were interleaved.
    if (Wire.endTransmission(true) != 0) return false;
    if (Wire.requestFrom(address, (uint8_t)1) != 1) return false;
    value = Wire.read();
    return true;
}

bool qmi8658ReadRegister(uint8_t reg, uint8_t &value) {
    return qmi8658Address != 0 &&
           qmi8658ReadRegisterAt(qmi8658Address, reg, value);
}

bool qmi8658ReadRegisters(uint8_t reg, uint8_t *values, uint8_t count) {
    if (qmi8658Address == 0 || values == nullptr || count == 0) return false;
    Wire.beginTransmission(qmi8658Address);
    Wire.write(reg);
    if (Wire.endTransmission(true) != 0) return false;
    if (Wire.requestFrom(qmi8658Address, count) != count) return false;
    for (uint8_t i = 0; i < count; ++i) values[i] = Wire.read();
    return true;
}

bool qmi8658WriteRegister(uint8_t reg, uint8_t value) {
    if (qmi8658Address == 0) return false;
    Wire.beginTransmission(qmi8658Address);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

bool qmi8658ExecuteCtrl9(uint8_t command) {
    if (!qmi8658WriteRegister(QMI8658_REG_CTRL9, command)) return false;
    const uint32_t startedMs = millis();
    while (millis() - startedMs < 100) {
        uint8_t statusInt = 0;
        if (qmi8658ReadRegister(QMI8658_REG_STATUS_INT, statusInt) &&
            (statusInt & QMI8658_STATUS_INT_CMD_DONE) != 0) {
            // Current QMI firmware requires NOP after STATUSINT.bit7 rises,
            // followed by waiting for the bit to clear. This is the protocol
            // used by Waveshare's current driver.
            if (!qmi8658WriteRegister(QMI8658_REG_CTRL9,
                                      QMI8658_CTRL9_NOP)) return false;
            const uint32_t ackStartedMs = millis();
            while (millis() - ackStartedMs < 100) {
                if (qmi8658ReadRegister(QMI8658_REG_STATUS_INT, statusInt) &&
                    (statusInt & QMI8658_STATUS_INT_CMD_DONE) == 0) {
                    return true;
                }
                delay(1);
            }
            return false;
        }

        uint8_t status1 = 0;
        if (qmi8658ReadRegister(QMI8658_REG_STATUS1, status1) &&
            (status1 & QMI8658_STATUS1_CMD_DONE) != 0) {
            // Backward compatibility for older revisions whose STATUS1 read
            // itself acknowledges CmdDone and releases INT1.
            return true;
        }
        delay(1);
    }
    return false;
}

bool configureMotionWakeSensor() {
    const uint8_t womControl = QMI8658_WOM_BLANKING_SAMPLES & 0x3F;
    return qmi8658WriteRegister(QMI8658_REG_CTRL7, 0x00) &&
           qmi8658WriteRegister(QMI8658_REG_CTRL2,
                                QMI8658_ACCEL_21HZ_LOW_POWER) &&
           qmi8658WriteRegister(QMI8658_REG_CAL1_L,
                                QMI8658_WOM_THRESHOLD_MG) &&
           qmi8658WriteRegister(QMI8658_REG_CAL1_H, womControl) &&
           qmi8658ExecuteCtrl9(QMI8658_CTRL9_WRITE_WOM) &&
           qmi8658WriteRegister(QMI8658_REG_CTRL7, 0x01);
}

bool configureMotionTrackingSensor() {
    // Threshold zero exits the one-shot WoM mode and returns INT1 inactive.
    // Normal operation then reads only the low-power accelerometer at 10 Hz;
    // the gyro, UI task and radio tasks are not touched by this polling path.
    return qmi8658WriteRegister(QMI8658_REG_CTRL7, 0x00) &&
           qmi8658WriteRegister(QMI8658_REG_CTRL2,
                                QMI8658_ACCEL_21HZ_LOW_POWER) &&
           qmi8658WriteRegister(QMI8658_REG_CAL1_L, 0x00) &&
           qmi8658WriteRegister(QMI8658_REG_CAL1_H, 0x00) &&
           qmi8658ExecuteCtrl9(QMI8658_CTRL9_WRITE_WOM) &&
           qmi8658WriteRegister(QMI8658_REG_CTRL7, 0x01);
}

bool initializeMotionWake() {
    pinMode(IMU_INT_PIN, INPUT);
    qmi8658Address = 0;
    for (const uint8_t address : {QMI8658_ADDRESS_HIGH,
                                  QMI8658_ADDRESS_LOW}) {
        uint8_t whoAmI = 0;
        if (qmi8658ReadRegisterAt(address, QMI8658_REG_WHO_AM_I, whoAmI) &&
            whoAmI == QMI8658_WHO_AM_I) {
            qmi8658Address = address;
            break;
        }
    }
    if (qmi8658Address == 0) {
        Serial.println("[STANDBY] QMI8658 not found; auto-standby disabled");
        return false;
    }

    // QMI8658A defaults INT1 to high impedance. GPIO8 cannot receive the WoM
    // level unless INT1 is explicitly switched to push-pull output mode.
    uint8_t ctrl1 = 0;
    if (!qmi8658ReadRegister(QMI8658_REG_CTRL1, ctrl1) ||
        !qmi8658WriteRegister(
            QMI8658_REG_CTRL1,
            ctrl1 | QMI8658_CTRL1_INT1_PUSH_PULL |
                QMI8658_CTRL1_AUTO_INCREMENT)) {
        Serial.println("[STANDBY] QMI8658 INT1 setup failed; auto-standby disabled");
        qmi8658Address = 0;
        return false;
    }

    // Clear a retained WoM/CmdDone condition after a deep-sleep wake or reset.
    uint8_t ignoredStatus = 0;
    qmi8658ReadRegister(QMI8658_REG_STATUS1, ignoredStatus);

    // Data-sheet WoM sequence: disable sensors, select +/-2 g at 21 Hz low
    // power, set threshold and INT1 initial-low routing, commit through CTRL9,
    // then enable only the accelerometer. Eight samples blank startup noise.
    if (!configureMotionTrackingSensor()) {
        Serial.println("[STANDBY] QMI8658 motion tracking setup failed; auto-standby disabled");
        qmi8658Address = 0;
        return false;
    }

    lastMotionActivityMs = millis();
    Serial.printf("[STANDBY] QMI8658 0x%02X physical motion ready on GPIO%u, timeout=5 min\n",
                  qmi8658Address, IMU_INT_PIN);
    return true;
}

void noteMotionActivity() {
    lastMotionActivityMs = millis();
}

void updateMotionActivity() {
    if (!imuMotionWakeAvailable || standbyEntering) return;
    static uint32_t lastSampleMs = 0;
    static bool haveReferenceSample = false;
    static int16_t reference[3] = {};
    const uint32_t nowMs = millis();
    if (nowMs - lastSampleMs < IMU_ACTIVITY_SAMPLE_MS) return;
    lastSampleMs = nowMs;

    uint8_t data[6] = {};
    if (!qmi8658ReadRegisters(QMI8658_REG_AX_L, data, sizeof(data))) return;
    const int16_t current[3] = {
        (int16_t)((uint16_t)data[1] << 8 | data[0]),
        (int16_t)((uint16_t)data[3] << 8 | data[2]),
        (int16_t)((uint16_t)data[5] << 8 | data[4])
    };
    if (haveReferenceSample) {
        // Compare against the position at the last accepted movement. Do not
        // move the reference on every sample: otherwise a slow pickup can be
        // split into tiny steps and never reset the inactivity timer.
        const int32_t dx = (int32_t)current[0] - reference[0];
        const int32_t dy = (int32_t)current[1] - reference[1];
        const int32_t dz = (int32_t)current[2] - reference[2];
        const int64_t thresholdSquared =
            (int64_t)IMU_ACTIVITY_DELTA_LSB * IMU_ACTIVITY_DELTA_LSB;
        const int64_t deltaSquared = (int64_t)dx * dx +
                                     (int64_t)dy * dy +
                                     (int64_t)dz * dz;
        if (deltaSquared >= thresholdSquared) {
            noteMotionActivity();
            reference[0] = current[0];
            reference[1] = current[1];
            reference[2] = current[2];
        }
    } else {
        reference[0] = current[0];
        reference[1] = current[1];
        reference[2] = current[2];
        haveReferenceSample = true;
    }
}

void restoreFromStandbyAbort() {
    standbyEntering = false;
    noteMotionActivity();
    requestedScreenOn = true;
    screenPowerRevision = screenPowerRevision + 1;
    if (uiTaskHandle) xTaskNotifyGive(uiTaskHandle);
}

void enterMotionStandby() {
    if (!imuMotionWakeAvailable || standbyEntering) return;

    // A motion event may have arrived between the last 100 ms status poll and
    // this function. Consume it and remain awake instead of racing sleep.
    uint8_t status = 0;
    if (qmi8658ReadRegister(QMI8658_REG_STATUS1, status) &&
        (status & QMI8658_STATUS1_WOM) != 0) {
        noteMotionActivity();
        configureMotionTrackingSensor();
        return;
    }

    standbyEntering = true;
    fakePulseMode = false;
    fakePressureOffset = 0.0f;
    standbyRetainedSettings = {
        STANDBY_SETTINGS_MAGIC,
        (uint16_t)fakeBpm,
        (uint16_t)fakeMaxPressureMmHg,
        (uint16_t)fakeMinPressureMmHg,
        (uint8_t)(darkTheme ? 1 : 0)
    };

    // Let the UI task issue the panel sleep command on its own dedicated core.
    requestedScreenOn = false;
    screenPowerRevision = screenPowerRevision + 1;
    if (uiTaskHandle) xTaskNotifyGive(uiTaskHandle);

    // Reject all new triggers and allow the last already-transmitted,
    // future-dated event to finish before the radios and CPUs power down.
    const int64_t drainUntilUs = lastTriggerExecuteAtUs == 0
        ? esp_timer_get_time() + 100000
        : lastTriggerExecuteAtUs + LARGE_PULSE_DURATION_US + 20000;
    while (esp_timer_get_time() < drainUntilUs) delay(10);

    // Movement during the short drain window cancels standby cleanly.
    status = 0;
    if (digitalRead(IMU_INT_PIN) == HIGH ||
        (qmi8658ReadRegister(QMI8658_REG_STATUS1, status) &&
         (status & QMI8658_STATUS1_WOM) != 0)) {
        configureMotionTrackingSensor();
        restoreFromStandbyAbort();
        return;
    }

    // The panel power transition can couple a one-off transient into the IMU.
    // Rearm only after the display is off, then wait out the configured eight
    // low-power accelerometer blanking samples before arming ESP deep sleep.
    if (!configureMotionWakeSensor()) {
        Serial.println("[STANDBY] QMI8658 sleep rearm failed");
        restoreFromStandbyAbort();
        return;
    }
    delay(QMI8658_WOM_REARM_DELAY_MS);
    status = 0;
    if (digitalRead(IMU_INT_PIN) == HIGH ||
        (qmi8658ReadRegister(QMI8658_REG_STATUS1, status) &&
         (status & QMI8658_STATUS1_WOM) != 0)) {
        configureMotionTrackingSensor();
        restoreFromStandbyAbort();
        return;
    }

    const esp_err_t wakeResult =
        esp_sleep_enable_ext0_wakeup((gpio_num_t)IMU_INT_PIN, HIGH);
    if (wakeResult != ESP_OK) {
        Serial.printf("[STANDBY] GPIO%u wake setup failed: %d\n",
                      IMU_INT_PIN, (int)wakeResult);
        restoreFromStandbyAbort();
        return;
    }

    Serial.println("[STANDBY] no motion for 5 min; display/radios/triggers off");
    Serial.flush();
    // Deep sleep stops both cores and the Wi-Fi/BLE radio domains. QMI8658
    // remains powered in accelerometer-only WoM mode and raises GPIO8 on motion.
    esp_deep_sleep_start();
}

void updateTouchGesture() {
    static bool tracking = false;
    static bool holdHandled = false;
    static int16_t startX = 0, startY = 0;
    static int16_t lastX = 0, lastY = 0;
    static uint32_t startedMs = 0;
    static uint32_t lastPollMs = 0;
    static uint32_t retryAfterMs = 0;
    static uint8_t consecutiveFailures = 0;
    const uint32_t nowMs = millis();

    if (!touchAvailable) {
        if ((int32_t)(nowMs - retryAfterMs) < 0) return;
        touchAvailable = initializeTouchBus(true);
        retryAfterMs = nowMs + TOUCH_RETRY_INTERVAL_MS;
        if (!touchAvailable) return;
        consecutiveFailures = 0;
        Serial.println("[TOUCH] I2C recovered");
    }

    if (nowMs - lastPollMs < TOUCH_POLL_INTERVAL_MS) return;
    lastPollMs = nowMs;

    int16_t x = 0, y = 0;
    bool pressed = false;
    if (!readTouchPoint(x, y, pressed)) {
        tracking = false;
        if (++consecutiveFailures >= TOUCH_FAILURE_LIMIT) {
            consecutiveFailures = 0;
            touchAvailable = false;
            retryAfterMs = nowMs + TOUCH_RETRY_INTERVAL_MS;
            // Keep the shared bus alive for QMI8658. The scheduled recovery
            // still performs an end/begin pair atomically in initializeTouchBus.
            Serial.println("[TOUCH] I2C paused after repeated errors");
        }
        return;
    }
    consecutiveFailures = 0;
    if (pressed) {
        noteMotionActivity();
        if (!tracking) {
            tracking = true;
            holdHandled = false;
            startX = lastX = x;
            startY = lastY = y;
            startedMs = nowMs;
        } else {
            lastX = x;
            lastY = y;
        }
        if (!holdHandled && nowMs - startedMs >= SCREEN_HOLD_TOGGLE_MS) {
            holdHandled = true;
            requestedScreenOn = !requestedScreenOn;
            screenPowerRevision = screenPowerRevision + 1;
            if (uiTaskHandle) xTaskNotifyGive(uiTaskHandle);
            Serial.printf("[UI] screen=%s\n",
                          requestedScreenOn ? "on" : "off");
        }
        return;
    }
    if (!tracking) return;
    tracking = false;
    // Do not interpret the release of a completed long press as a swipe.
    if (holdHandled) return;
    const int16_t dx = lastX - startX;
    const int16_t dy = lastY - startY;
    if (nowMs - startedMs > SWIPE_MAX_DURATION_MS ||
        abs(dx) < SWIPE_MIN_DISTANCE || abs(dx) <= abs(dy)) return;

    // Light: right-to-left enters dark mode. Dark: left-to-right returns.
    const bool requestedDark = dx < 0;
    if (requestedDark == darkTheme) return;
    darkTheme = requestedDark;
    themeRevision = themeRevision + 1;
    if (uiTaskHandle) xTaskNotifyGive(uiTaskHandle);
    Serial.printf("[UI] theme=%s\n", requestedDark ? "dark" : "light");
}

bool beginUi() {
    bus = new Arduino_ESP32QSPI(LCD_CS, LCD_PCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3, false);
    display = new Arduino_CO5300(bus, LCD_RST, 0, SCREEN_W, SCREEN_H, 6, 0, 6, 0);
    if (!display || !display->begin(80000000)) return false;
    canvas = new Arduino_Canvas(SCREEN_W, SCREEN_H, display);
    if (!canvas || !canvas->begin(GFX_SKIP_OUTPUT_BEGIN)) return false;
    const size_t frameBytes = (size_t)SCREEN_W * SCREEN_H * sizeof(uint16_t);
    backgroundPixels = static_cast<uint16_t *>(
        heap_caps_malloc(frameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    dirtyPixels = static_cast<uint16_t *>(
        heap_caps_malloc(frameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!backgroundPixels || !dirtyPixels) return false;

    drawStaticGauge();
    canvas->flush();
    memcpy(backgroundPixels, canvas->getFramebuffer(), frameBytes);
    return true;
}

#if 0  // Real cuff-pulse detection removed; synthetic BPM is the only source.
void updatePulseDetector(float pressure, bool saturated, uint32_t nowMs) {
    // Detection filtering is separate from the needle path: the needle still
    // receives the unfiltered scaled sample. This extracts only the short AC
    // cuff oscillation riding on top of the slowly changing cuff pressure.
    static bool initialized = false;
    static bool candidateActive = false;
    static bool waitingForRearm = false;
    static uint32_t initializedAtMs = 0;
    static uint32_t lastSampleMs = 0;
    static uint32_t trendStartedMs = 0;
    static uint32_t candidateStartedMs = 0;
    static uint32_t lastPulseMs = 0;
    static uint32_t rearmStartedMs = 0;
    static float baseline = 0.0f;
    static float filteredOscillation = 0.0f;
    static float trendReference = 0.0f;
    static float trendRate = 99.0f;
    static float candidatePeak = 0.0f;

    // No useful pulse decision at very low cuff pressure or while the ADC is
    // clipped. Re-arm from the current level after pressure becomes valid.
    if (pressure < 20.0f || saturated) {
        initialized = false;
        candidateActive = false;
        waitingForRearm = false;
        return;
    }

    if (!initialized) {
        initialized = true;
        initializedAtMs = nowMs;
        lastSampleMs = nowMs;
        trendStartedMs = nowMs;
        baseline = pressure;
        trendReference = pressure;
        trendRate = 99.0f;
        filteredOscillation = 0.0f;
        candidateActive = false;
        waitingForRearm = false;
        return;
    }

    float dt = (nowMs - lastSampleMs) * 0.001f;
    lastSampleMs = nowMs;
    dt = constrain(dt, 0.001f, 0.100f);

    // About 0.35 Hz high-pass followed by an 8 Hz low-pass. This follows slow
    // cuff drift but retains the heartbeat band at the 500 SPS ADC rate.
    constexpr float baselineTauSeconds = 0.455f;
    constexpr float lowPassTauSeconds = 0.020f;
    baseline += (dt / (baselineTauSeconds + dt)) * (pressure - baseline);
    const float oscillation = pressure - baseline;
    filteredOscillation += (dt / (lowPassTauSeconds + dt)) *
                           (oscillation - filteredOscillation);

    const uint32_t trendElapsedMs = nowMs - trendStartedMs;
    if (trendElapsedMs >= 500) {
        trendRate = (baseline - trendReference) * 1000.0f / trendElapsedMs;
        trendReference = baseline;
        trendStartedMs = nowMs;
    }

    // Wait for inflation/deflation to settle. A locked cuff should have only a
    // small baseline drift; a pump or open valve is rejected here.
    const bool pressureHeld = nowMs - initializedAtMs >= 800 &&
                              fabsf(trendRate) <= 3.0f;

    // Do not let a reflected second positive lobe re-open a candidate. Wait
    // for the waveform to cross below its baseline first. The timeout avoids
    // getting stuck if a very asymmetric pulse never quite crosses zero.
    if (waitingForRearm) {
        if (filteredOscillation <= -0.03f ||
            nowMs - rearmStartedMs >= 700) {
            waitingForRearm = false;
        }
        return;
    }

    if (!candidateActive) {
        if (pressureHeld &&
            filteredOscillation >= PULSE_MIN_AMPLITUDE_MMHG &&
            filteredOscillation <= PULSE_MAX_AMPLITUDE_MMHG) {
            candidateActive = true;
            candidateStartedMs = nowMs;
            candidatePeak = filteredOscillation;
        }
        return;
    }

    candidatePeak = max(candidatePeak, filteredOscillation);
    const uint32_t candidateAgeMs = nowMs - candidateStartedMs;

    if (!pressureHeld || candidatePeak > PULSE_MAX_AMPLITUDE_MMHG ||
        candidateAgeMs > 900) {
        candidateActive = false;
        return;
    }

    const float returnThreshold = max(0.04f, candidatePeak * 0.25f);
    const bool returnedToBaseline = filteredOscillation <= returnThreshold;
    if (candidateAgeMs >= 80 && returnedToBaseline) {
        if (nowMs - lastPulseMs >= REAL_PULSE_REFRACTORY_MS) {
            lastPulseMs = nowMs;
            lastPulseAmplitude = candidatePeak;
            lastRealPulseMs = nowMs;
            fakePressureOffset = 0.0f;
            fakePulseMode = false;
            detectedPulseCount = detectedPulseCount + 1;
            if (sendPulseTrigger()) {
                visiblePulseCount = visiblePulseCount + 1;
                if (uiTaskHandle) xTaskNotifyGive(uiTaskHandle);
            }
            waitingForRearm = true;
            rearmStartedMs = nowMs;
        }
        candidateActive = false;
    }
}
#endif

void updateFakePulse(float realPressure, bool saturated, uint32_t nowMs) {
    constexpr uint8_t DISPLAY_PULSE_QUEUE_SIZE = 8;
    static uint32_t nextBeatMs = 0;
    static uint32_t outOfRangeSinceMs = 0;
    static uint32_t appliedBpmRevision = 0;
    static int64_t scheduledDisplayUs[DISPLAY_PULSE_QUEUE_SIZE] = {};
    static uint8_t displayHead = 0;
    static uint8_t displayCount = 0;
    static int64_t displayedBeatAtUs = 0;
    static bool rangeActive = false;

    const auto clearScheduledDisplay = [&]() {
        displayHead = 0;
        displayCount = 0;
        displayedBeatAtUs = 0;
    };

    if (otaServiceInProgress()) {
        rangeActive = false;
        nextBeatMs = 0;
        outOfRangeSinceMs = 0;
        clearScheduledDisplay();
        fakePulseMode = false;
        fakePressureOffset = 0.0f;
        return;
    }

    if (appliedBpmRevision != fakeBpmRevision) {
        appliedBpmRevision = fakeBpmRevision;
        nextBeatMs = 0;
        outOfRangeSinceMs = 0;
        rangeActive = false;
        clearScheduledDisplay();
        fakePressureOffset = 0.0f;
    }

    // Synthetic BPM exists only strictly between the BLE diastolic and
    // systolic values: DIA < measured mmHg < SYS.
    const bool rawPressureInRange = !saturated &&
        realPressure > (float)fakeMinPressureMmHg &&
        realPressure < (float)fakeMaxPressureMmHg;
    if (!rangeActive) {
        if (!rawPressureInRange) {
            fakePressureOffset = 0.0f;
            fakePulseMode = false;
            return;
        }
        // Entering the configured range starts the first trigger immediately.
        rangeActive = true;
        outOfRangeSinceMs = 0;
        nextBeatMs = nowMs;
    } else if (rawPressureInRange) {
        outOfRangeSinceMs = 0;
    } else {
        if (outOfRangeSinceMs == 0) outOfRangeSinceMs = nowMs;
        if (nowMs - outOfRangeSinceMs >= FAKE_RANGE_EXIT_DEBOUNCE_MS) {
            const bool exitedAboveUpper =
                realPressure > (float)fakeMaxPressureMmHg || saturated;
            rangeActive = false;
            outOfRangeSinceMs = 0;
            nextBeatMs = 0;
            clearScheduledDisplay();
            fakePressureOffset = 0.0f;
            fakePulseMode = false;
            if (exitedAboveUpper) {
                // Cancel a UI event already queued for the old valid range.
                fakeUiCancelRevision = fakeUiCancelRevision + 1;
            }
            return;
        }
    }

    // Keep the 300 ms debounce only for the beat timeline. Ripple and UI LED
    // visibility follow the strict pressure bounds immediately, so a pulse
    // cannot remain visible at/below DIA or above SYS while the cadence state
    // waits to see whether this was merely boundary noise.
    fakePulseMode = rawPressureInRange;
    if (!rawPressureInRange) {
        displayedBeatAtUs = 0;
        fakePressureOffset = 0.0f;
    }

    const uint16_t bpm = constrain((uint16_t)fakeBpm,
                                   (uint16_t)30, (uint16_t)240);
    const uint32_t periodMs = 60000UL / bpm;
    const int64_t nowUs = esp_timer_get_time();
    if (nextBeatMs == 0) nextBeatMs = nowMs;
    if ((int32_t)(nowMs - nextBeatMs) >= 0) {
        nextBeatMs += periodMs;
        if ((int32_t)(nowMs - nextBeatMs) >= 0) nextBeatMs = nowMs + periodMs;
        int64_t executeAtUs = 0;
        uint32_t eventId = 0;
        if (rawPressureInRange &&
            displayCount < DISPLAY_PULSE_QUEUE_SIZE &&
            sendPulseTrigger(PULSE_LARGE, 0, &executeAtUs, &eventId)) {
            const uint8_t tail = (displayHead + displayCount) %
                                 DISPLAY_PULSE_QUEUE_SIZE;
            scheduledDisplayUs[tail] = executeAtUs;
            ++displayCount;
        }
    }

    // Drain acknowledgements; all outputs and the monitor still start from
    // the same master executeAt timestamp. Waiting for the first ACK here
    // would make the display follow one transport and de-sync from the other.
    WifiLedAck wifiAck{};
    while (wifiTakeLedAck(wifiAck)) {}
    BleLedAck bleAck{};
    while (bleTakeLedAck(bleAck)) {}

    while (displayCount > 0 &&
           nowUs >= scheduledDisplayUs[displayHead]) {
        const int64_t dueDisplayUs = scheduledDisplayUs[displayHead];
        displayHead = (displayHead + 1) % DISPLAY_PULSE_QUEUE_SIZE;
        --displayCount;
        if (rawPressureInRange) {
            displayedBeatAtUs = dueDisplayUs;
            visiblePulseCount = visiblePulseCount + 1;
            if (uiTaskHandle) xTaskNotifyGive(uiTaskHandle);
        } else {
            // Consume an already scheduled visual event while hidden. It must
            // not reappear as a partial ripple when pressure re-enters range.
            displayedBeatAtUs = 0;
        }
    }

    // Every beat uses the same fixed 1.5 mmHg peak. Duration scales with the
    // configured BPM so high rates cannot overlap into a continuous wobble.
    const uint32_t pulseDurationMs = min<uint32_t>(520, periodMs * 65 / 100);
    const int64_t ageUs = nowUs - displayedBeatAtUs;
    if (rawPressureInRange && displayedBeatAtUs != 0 && ageUs >= 0 &&
        ageUs < (int64_t)pulseDurationMs * 1000) {
        const float phase = (float)ageUs /
                            ((float)pulseDurationMs * 1000.0f);
        // The physical LEDs turn on at displayedBeatAtUs. Put the dial peak at
        // that exact timestamp as well, then return smoothly to baseline.
        // The previous full raised cosine peaked halfway through the pulse,
        // making the dial appear roughly 260 ms late.
        fakePressureOffset = FAKE_PULSE_AMPLITUDE_MMHG *
                             0.5f * (1.0f + cosf(PI * phase));
    } else {
        fakePressureOffset = 0.0f;
    }
}

float medianPressureSample(float sample) {
    // Fixed-size stack/ring buffers only: at 500 Hz this is at most ten tiny
    // comparisons per sample and never allocates or touches the UI core.
    static float samples[PRESSURE_MEDIAN_WINDOW] = {};
    static uint8_t nextSample = 0;
    static bool initialized = false;
    if (!initialized) {
        for (float &stored : samples) stored = sample;
        initialized = true;
    } else {
        samples[nextSample] = sample;
        nextSample = (nextSample + 1) % PRESSURE_MEDIAN_WINDOW;
    }

    float ordered[PRESSURE_MEDIAN_WINDOW];
    memcpy(ordered, samples, sizeof(ordered));
    for (uint8_t i = 1; i < PRESSURE_MEDIAN_WINDOW; ++i) {
        const float value = ordered[i];
        int8_t j = (int8_t)i - 1;
        while (j >= 0 && ordered[j] > value) {
            ordered[j + 1] = ordered[j];
            --j;
        }
        ordered[j + 1] = value;
    }
    return ordered[PRESSURE_MEDIAN_WINDOW / 2];
}

float rejectTransientPressureSpike(float sample) {
    static bool initialized = false;
    static bool candidateActive = false;
    static float acceptedPressure = 0.0f;
    static float previousSample = 0.0f;
    static int8_t candidateDirection = 0;
    static uint8_t candidateSamples = 0;

    if (!initialized) {
        initialized = true;
        acceptedPressure = sample;
        previousSample = sample;
        return sample;
    }

    const float adjacentStep = sample - previousSample;
    previousSample = sample;
    if (!candidateActive) {
        if (fabsf(adjacentStep) <= PRESSURE_SPIKE_STEP_MMHG) {
            acceptedPressure = sample;
            return acceptedPressure;
        }
        candidateActive = true;
        candidateDirection = adjacentStep > 0.0f ? 1 : -1;
        candidateSamples = 1;
        return acceptedPressure;
    }

    const float distanceFromAccepted = sample - acceptedPressure;
    if (fabsf(distanceFromAccepted) <= PRESSURE_SPIKE_RETURN_MMHG) {
        // It returned to the old signal before confirmation: discard the
        // complete excursion instead of letting even one UI frame see it.
        candidateActive = false;
        candidateSamples = 0;
        acceptedPressure = sample;
        return acceptedPressure;
    }

    const bool sameDirection = candidateDirection > 0
        ? distanceFromAccepted > 0.0f
        : distanceFromAccepted < 0.0f;
    if (!sameDirection) {
        candidateDirection = distanceFromAccepted > 0.0f ? 1 : -1;
        candidateSamples = 1;
        return acceptedPressure;
    }

    const bool remainsBeyondStep = candidateDirection > 0
        ? distanceFromAccepted >= PRESSURE_SPIKE_STEP_MMHG
        : distanceFromAccepted <= -PRESSURE_SPIKE_STEP_MMHG;
    if (!remainsBeyondStep) {
        // The remaining change is below the artifact threshold. Resume normal
        // tracking rather than turning a legitimate slow cuff ramp into steps.
        candidateActive = false;
        candidateSamples = 0;
        acceptedPressure = sample;
        return acceptedPressure;
    }

    if (++candidateSamples >= PRESSURE_SPIKE_CONFIRM_SAMPLES) {
        // A persistent transition is real inflation/deflation. Together with
        // the median window its added latency is about 20 ms, below one 30 Hz
        // UI frame.
        candidateActive = false;
        candidateSamples = 0;
        acceptedPressure = sample;
    }
    return acceptedPressure;
}

void processSample(int32_t raw, uint32_t milliVolts) {
    // Discard mode-transition samples before calculating the empty-cuff zero.
    if (warmupSamples < WARMUP_SAMPLE_COUNT) {
        ++warmupSamples;
        return;
    }
    if (zeroSamples < ZERO_SAMPLE_COUNT) {
        zeroRawSum += raw;
        zeroMilliVoltsSum += milliVolts;
        const uint16_t newZeroSampleCount = zeroSamples + 1;
        zeroSamples = newZeroSampleCount;
        if (newZeroSampleCount == ZERO_SAMPLE_COUNT) {
            zeroRaw = static_cast<int32_t>(zeroRawSum / newZeroSampleCount);
            zeroMilliVolts = static_cast<uint32_t>(
                zeroMilliVoltsSum / newZeroSampleCount);
            // Never learn an inflated cuff as the zero point. The sensor's
            // empty-cuff output is nominally 200 mV; a much higher startup
            // average (913 mV was observed) would clamp all later readings to
            // zero and make the configured BPM appear far too slow.
            if (zeroMilliVolts < SENSOR_ZERO_MIN_VALID_MV ||
                zeroMilliVolts > SENSOR_ZERO_MAX_VALID_MV) {
                Serial.printf("[SENSOR] rejected startup zero=%lu mV, using nominal=%u mV\n",
                              (unsigned long)zeroMilliVolts,
                              (unsigned)SENSOR_NOMINAL_ZERO_MV);
                zeroMilliVolts = SENSOR_NOMINAL_ZERO_MV;
            }
        }
        return;
    }

    // The empty-cuff voltage is measured at startup. Nominal transfer:
    // 0.2 V = 0 kPa and 2.7 V = 40 kPa = 300.02 mmHg.
    const float usableSpanMv = max(1.0f,
        (float)SENSOR_FULL_SCALE_MV - (float)zeroMilliVolts);
    float p = ((float)milliVolts - (float)zeroMilliVolts) *
              SENSOR_FULL_SCALE_MMHG / usableSpanMv;
    p *= PRESSURE_GAIN_CORRECTION;
    p = constrain(p, -20.0f, GAUGE_MAX_MMHG);
    const float cleanPressure = rejectTransientPressureSpike(
        medianPressureSample(p));
    latestRaw = raw;
    latestMilliVolts = milliVolts;
    adcSaturated = raw >= ADC_RESOLUTION_MAX - 3 || milliVolts >= 3050;
    // UI and synthetic range decisions share the cleaned physical pressure.
    // The fixed synthetic ripple is still added afterwards in updateUi().
    targetPressure = max(0.0f, cleanPressure);
    const uint32_t nowMs = millis();
    updateFakePulse(targetPressure, adcSaturated, nowMs);
}

void updateUi() {
    static uint32_t displayedPulseCount = 0;
    static uint32_t pulseLedUntilMs = 0;
    static bool pulseLedOn = false;
    static uint32_t appliedCancelRevision = 0;
    static float filteredUiBasePressure = -1.0f;
    static uint32_t appliedScreenPowerRevision = 0;
    static bool screenOn = true;
    const uint32_t requestedPowerRevision = screenPowerRevision;
    if (requestedPowerRevision != appliedScreenPowerRevision) {
        appliedScreenPowerRevision = requestedPowerRevision;
        screenOn = requestedScreenOn;
        if (screenOn) {
            display->displayOn();
            // Restore the complete framebuffer once after waking. Normal
            // operation immediately returns to dirty-region updates.
            canvas->flush();
            lastNeedlePressure = -1.0f;
        } else {
            display->displayOff();
        }
    }
    if (!screenOn) return;

    static uint32_t appliedThemeRevision = 0;
    const uint32_t requestedThemeRevision = themeRevision;
    const bool themeChanged = requestedThemeRevision != appliedThemeRevision;
    if (themeChanged) {
        appliedThemeRevision = requestedThemeRevision;
        drawStaticGauge();
        const size_t frameBytes =
            (size_t)SCREEN_W * SCREEN_H * sizeof(uint16_t);
        memcpy(backgroundPixels, canvas->getFramebuffer(), frameBytes);
        canvas->flush();
        lastNeedlePressure = -1.0f;
        // The freshly rebuilt background already contains the unlit LED.
        // A currently active beat will redraw it below in the new palette.
        pulseLedOn = false;
    }

    // Re-check the pressure at draw time. This prevents a previously queued
    // display beat from appearing after the cuff has fallen out of range,
    // even if the sensor task and UI task cross between two frames.
    const float basePressure = (float)targetPressure;
    if (filteredUiBasePressure < 0.0f) {
        filteredUiBasePressure = basePressure;
    } else {
        const float difference = basePressure - filteredUiBasePressure;
        // Strong filtering for ADC jitter, faster tracking while the cuff is
        // actively inflated/deflated. The synthetic pulse is added afterwards
        // so its timestamp and fixed 1.5 mmHg amplitude are not attenuated.
        // These faster coefficients remove the former "low FPS" feeling while
        // retaining enough filtering to stop single-sample needle chatter.
        const float alpha = fabsf(difference) > 3.0f ? 0.65f : 0.20f;
        filteredUiBasePressure += difference * alpha;
        if (fabsf(basePressure - filteredUiBasePressure) < 0.04f) {
            filteredUiBasePressure = basePressure;
        }
    }
    displayedGaugeBasePressure = filteredUiBasePressure;
    // Final UI gate uses exactly the current measured value requested by the
    // BLE contract: DIA < measured mmHg < SYS. Otherwise the added fake offset
    // and its UI LED are both zero/hidden immediately.
    const float measuredPressure = (float)targetPressure;
    const bool fakeVisible = fakePulseMode &&
        (float)fakeMinPressureMmHg < measuredPressure &&
        measuredPressure < (float)fakeMaxPressureMmHg;
    const float pressure = constrain(filteredUiBasePressure +
                                     (fakeVisible
                                          ? constrain((float)fakePressureOffset, 0.0f,
                                                      FAKE_PULSE_AMPLITUDE_MMHG)
                                          : 0.0f),
                                     0.0f, GAUGE_MAX_MMHG);

    const float oldPressure = lastNeedlePressure < 0.0f ? pressure : lastNeedlePressure;
    int16_t oldX, oldY, newX, newY;
    pointOnGauge(oldPressure, NEEDLE_RADIUS, oldX, oldY);
    pointOnGauge(pressure, NEEDLE_RADIUS, newX, newY);

    uint16_t *frame = canvas->getFramebuffer();
    if (oldX != newX || oldY != newY || lastNeedlePressure < 0.0f) {
        constexpr int16_t margin = 9;
        int16_t x1 = min(GAUGE_CX, min(oldX, newX)) - margin;
        int16_t y1 = min(GAUGE_CY, min(oldY, newY)) - margin;
        int16_t x2 = max(GAUGE_CX, max(oldX, newX)) + margin;
        int16_t y2 = max(GAUGE_CY, max(oldY, newY)) + margin;
        x1 = max((int16_t)0, x1); y1 = max((int16_t)0, y1);
        x2 = min((int16_t)(SCREEN_W - 1), x2);
        y2 = min((int16_t)(SCREEN_H - 1), y2);
        const int16_t width = x2 - x1 + 1;
        const int16_t height = y2 - y1 + 1;
        for (int16_t row = y1; row <= y2; ++row) {
            memcpy(frame + (size_t)row * SCREEN_W + x1,
                   backgroundPixels + (size_t)row * SCREEN_W + x1,
                   (size_t)width * sizeof(uint16_t));
        }
        const uint16_t needleRed = canvas->color565(212, 21, 32);
        const uint16_t hubDark = canvas->color565(90, 0, 0);
        drawThickLine(canvas, GAUGE_CX, GAUGE_CY, newX, newY, needleRed, 5);
        canvas->fillCircle(GAUGE_CX, GAUGE_CY, 15, hubDark);
        canvas->fillCircle(GAUGE_CX, GAUGE_CY, 11, needleRed);
        for (int16_t row = 0; row < height; ++row) {
            memcpy(dirtyPixels + (size_t)row * width,
                   frame + (size_t)(y1 + row) * SCREEN_W + x1,
                   (size_t)width * sizeof(uint16_t));
        }
        display->draw16bitRGBBitmap(x1, y1, dirtyPixels, width, height);
    }
    lastNeedlePressure = pressure;

    const uint32_t nowMs = millis();
    const uint32_t pulseCount = visiblePulseCount;
    const uint32_t cancelRevision = fakeUiCancelRevision;
    if (cancelRevision != appliedCancelRevision) {
        appliedCancelRevision = cancelRevision;
        displayedPulseCount = pulseCount;
        pulseLedUntilMs = 0;
    } else if (!fakeVisible) {
        // Consume any already-counted UI event while hidden so it cannot flash
        // late when pressure rises above 30 mmHg again.
        displayedPulseCount = pulseCount;
        pulseLedUntilMs = 0;
    } else if (pulseCount != displayedPulseCount) {
        displayedPulseCount = pulseCount;
        pulseLedUntilMs = nowMs + PULSE_LED_ON_MS;
    }

    const bool shouldLightLed = fakeVisible && displayedPulseCount != 0 &&
                                (int32_t)(pulseLedUntilMs - nowMs) > 0;
    if (shouldLightLed != pulseLedOn) {
        drawPulseLed(canvas, shouldLightLed);
        constexpr int16_t ledMargin = PULSE_LED_RADIUS + 4;
        constexpr int16_t ledX = PULSE_LED_CX - ledMargin;
        constexpr int16_t ledY = PULSE_LED_CY - ledMargin;
        constexpr int16_t ledSize = ledMargin * 2 + 1;

        for (int16_t row = 0; row < ledSize; ++row) {
            memcpy(dirtyPixels + (size_t)row * ledSize,
                   frame + (size_t)(ledY + row) * SCREEN_W + ledX,
                   (size_t)ledSize * sizeof(uint16_t));
        }
        display->draw16bitRGBBitmap(ledX, ledY, dirtyPixels,
                                    ledSize, ledSize);
        pulseLedOn = shouldLightLed;
    }

    // Battery indicator occupies the free lower-left area and is redrawn only
    // when its displayed value changes (normally once every five seconds).
    static uint8_t lastBatteryPercent = 0xff;
    static bool lastBatteryValid = false;
    static bool lastBatteryCharging = false;
    const uint8_t shownBatteryPercent = batteryPercent;
    const bool shownBatteryValid = batteryReadingValid;
    const bool shownBatteryCharging = batteryChargingEstimated;
    if (themeChanged || shownBatteryPercent != lastBatteryPercent ||
        shownBatteryValid != lastBatteryValid ||
        shownBatteryCharging != lastBatteryCharging) {
        lastBatteryPercent = shownBatteryPercent;
        lastBatteryValid = shownBatteryValid;
        lastBatteryCharging = shownBatteryCharging;
        constexpr int16_t batX = 188, batY = 433;
        constexpr int16_t batW = 91, batH = 13;
        const bool dark = darkTheme;
        const uint16_t face = canvas->color565(dark ? 5 : 244,
                                                dark ? 7 : 240,
                                                dark ? 9 : 230);
        const uint16_t ink = canvas->color565(dark ? 225 : 40,
                                               dark ? 228 : 45,
                                               dark ? 232 : 48);
        const uint16_t level = shownBatteryPercent > 20
                                   ? canvas->color565(25, 145, 65)
                                   : canvas->color565(190, 35, 30);
        canvas->fillRect(batX, batY, batW, batH, face);
        canvas->drawRect(batX, batY + 2, 20, 9, ink);
        canvas->fillRect(batX + 20, batY + 4, 3, 4, ink);
        if (shownBatteryValid) {
            const int16_t fillW = (int16_t)shownBatteryPercent * 16 / 100;
            if (fillW > 0) canvas->fillRect(batX + 2, batY + 4, fillW, 5, level);
        }
        canvas->setTextSize(1);
        canvas->setTextColor(ink);
        canvas->setCursor(batX + 29, batY + 4);
        if (shownBatteryValid) {
            canvas->printf("%u%%", (unsigned)shownBatteryPercent);
            if (shownBatteryCharging) canvas->print(" CHG");
        } else {
            canvas->print("BAT --");
        }
        for (int16_t row = 0; row < batH; ++row) {
            memcpy(dirtyPixels + (size_t)row * batW,
                   frame + (size_t)(batY + row) * SCREEN_W + batX,
                   (size_t)batW * sizeof(uint16_t));
        }
        display->draw16bitRGBBitmap(batX, batY, dirtyPixels, batW, batH);
    }

    // Compact connection strip in the otherwise unused area below the pulse
    // LED. It is redrawn last so the moving needle cannot erase it.
    const WifiNodeStatus wifi = wifiNodeStatus(WIFI_NODE_ID);
    const BleNodeStatus ble = bleNodeStatus();
    static uint8_t lastStatusBits = 0xff;
    const uint8_t statusBits = (wifi.connected ? 0x01 : 0) |
                               (wifi.synced ? 0x02 : 0) |
                               (ble.connected ? 0x04 : 0) |
                               (ble.synced ? 0x08 : 0);
    if (themeChanged || statusBits != lastStatusBits) {
        lastStatusBits = statusBits;
    constexpr int16_t statusX = 195, statusY = 449;
    constexpr int16_t statusW = 76, statusH = 13;
    const bool dark = darkTheme;
    const uint16_t face = canvas->color565(dark ? 5 : 244,
                                            dark ? 7 : 240,
                                            dark ? 9 : 230);
    const uint16_t ok = canvas->color565(dark ? 65 : 0,
                                          dark ? 220 : 120,
                                          dark ? 105 : 45);
    const uint16_t bad = canvas->color565(dark ? 255 : 170,
                                           dark ? 80 : 25,
                                           dark ? 75 : 25);
    canvas->fillRect(statusX, statusY, statusW, statusH, face);
    canvas->setTextSize(1);
    canvas->setTextColor(wifi.synced ? ok : bad);
    canvas->setCursor(statusX, statusY + 2);
    canvas->print("W:");
    canvas->print(wifi.synced ? "OK" : (wifi.connected ? "SYNC" : "OFF"));
    canvas->setTextColor(ble.synced ? ok : bad);
    canvas->setCursor(statusX + 38, statusY + 2);
    canvas->print("B:");
    canvas->print(ble.synced ? "OK" : (ble.connected ? "SYNC" : "OFF"));
    for (int16_t row = 0; row < statusH; ++row) {
        memcpy(dirtyPixels + (size_t)row * statusW,
               frame + (size_t)(statusY + row) * SCREEN_W + statusX,
               (size_t)statusW * sizeof(uint16_t));
    }
    display->draw16bitRGBBitmap(statusX, statusY, dirtyPixels,
                                statusW, statusH);
    }
}

void sensorTask(void *) {
    TickType_t lastWakeTick = xTaskGetTickCount();
    const TickType_t samplePeriodTicks = pdMS_TO_TICKS(2);
    uint32_t rateWindowStarted = millis();
    uint16_t sampleCount = 0;

    for (;;) {
        const TickType_t sensorNowTick = xTaskGetTickCount();
        if ((int32_t)(sensorNowTick - lastWakeTick) >
            (int32_t)(samplePeriodTicks * 2)) {
            // Drop missed samples instead of running thousands of catch-up
            // iterations that temporarily starve the rest of the system.
            lastWakeTick = sensorNowTick;
        }
        // Block instead of busy-spinning between 500 Hz samples. This leaves
        // CPU time for the display and, most importantly, the radio stacks.
        vTaskDelayUntil(&lastWakeTick, samplePeriodTicks);

        // Two adjacent conversions expose both native 12-bit data and the
        // ESP32 calibration result in millivolts on the serial monitor.
        const int32_t raw = analogRead(PRESSURE_ADC_PIN);
        const uint32_t milliVolts = analogReadMilliVolts(PRESSURE_ADC_PIN);
        processSample(raw, milliVolts);
        ++sampleCount;

        const uint32_t now = millis();
        // Non-blocking: normally returns immediately and takes at most one
        // battery ADC sample on a scheduled pass.
        updateBatteryMeasurement();
        if (now - rateWindowStarted >= 1000) {
            measuredSps = (uint32_t)sampleCount * 1000 /
                          (now - rateWindowStarted);
            sampleCount = 0;
            rateWindowStarted = now;
        }
    }
}

void uiTask(void *) {
    const TickType_t framePeriodTicks = pdMS_TO_TICKS(1000 / UI_REFRESH_HZ);
    TickType_t lastPeriodicFrame = xTaskGetTickCount();
    uint32_t rateWindowStarted = millis();
    uint16_t frameCount = 0;

    for (;;) {
        // Keep an absolute 30 Hz timeline: drawing time is part of the 33 ms
        // frame budget instead of being added before every wait. A trigger can
        // wake us early without shifting subsequent periodic frames.
        TickType_t nowTick = xTaskGetTickCount();
        TickType_t deadline = lastPeriodicFrame + framePeriodTicks;
        TickType_t waitTicks = (int32_t)(deadline - nowTick) > 0
                                   ? deadline - nowTick
                                   : 0;
        const bool triggerWake =
            ulTaskNotifyTake(pdTRUE, waitTicks) > 0;
        nowTick = xTaskGetTickCount();
        if (!triggerWake || (int32_t)(nowTick - deadline) >= 0) {
            // Advance by one fixed period. If unexpectedly very late, discard
            // missed frames rather than burst-rendering them.
            lastPeriodicFrame = deadline;
            if ((int32_t)(nowTick - lastPeriodicFrame) >
                (int32_t)(framePeriodTicks * 2)) {
                lastPeriodicFrame = nowTick;
            }
        }

        const uint32_t drawStartedMs = millis();
        updateUi();
        const uint32_t drawMs = millis() - drawStartedMs;
        lastUiFrameMs = millis();
        if (drawMs > maxUiDrawMs) {
            maxUiDrawMs = (uint16_t)min<uint32_t>(drawMs, UINT16_MAX);
        }
        ++frameCount;

        const uint32_t now = millis();
        if (now - rateWindowStarted >= 1000) {
            measuredUiFps = (uint32_t)frameCount * 1000 /
                            (now - rateWindowStarted);
            frameCount = 0;
            rateWindowStarted = now;
        }

        // An early trigger frame may itself cross the pending periodic
        // deadline. Count it as that frame so we never draw twice back-to-back.
        if (triggerWake) {
            nowTick = xTaskGetTickCount();
            deadline = lastPeriodicFrame + framePeriodTicks;
            if ((int32_t)(nowTick - deadline) >= 0) {
                lastPeriodicFrame = deadline;
            }
        }
    }
}

void setup() {
    Serial.begin(115200);
    const esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
    Preferences settings;
    if (settings.begin("bp-sim", true)) {
        fakeBpm = constrain(settings.getUShort("bpm", DEFAULT_FAKE_BPM),
                            (uint16_t)30, (uint16_t)240);
        fakeMaxPressureMmHg = constrain(
            settings.getUShort("max_mmhg", DEFAULT_FAKE_MAX_PRESSURE_MMHG),
            (uint16_t)31, (uint16_t)300);
        fakeMinPressureMmHg = constrain(
            settings.getUShort("min_mmhg", DEFAULT_FAKE_MIN_PRESSURE_MMHG),
            (uint16_t)1, (uint16_t)(fakeMaxPressureMmHg - 1));
        settings.end();
    }
    if (wakeCause == ESP_SLEEP_WAKEUP_EXT0 &&
        standbyRetainedSettings.magic == STANDBY_SETTINGS_MAGIC) {
        fakeBpm = constrain(standbyRetainedSettings.bpm,
                            (uint16_t)30, (uint16_t)240);
        fakeMaxPressureMmHg = constrain(standbyRetainedSettings.systolicMmHg,
                                        (uint16_t)31, (uint16_t)300);
        fakeMinPressureMmHg = constrain(
            standbyRetainedSettings.diastolicMmHg,
            (uint16_t)1, (uint16_t)(fakeMaxPressureMmHg - 1));
        darkTheme = standbyRetainedSettings.darkTheme != 0;
        Serial.println("[STANDBY] motion wake; restoring normal operation");
    }
    Serial.printf("[FAKE PULSE] current BPM=%u range=%u-%u mmHg\n",
                  (unsigned)fakeBpm, (unsigned)fakeMinPressureMmHg,
                  (unsigned)fakeMaxPressureMmHg);
    pinMode(PRESSURE_ADC_PIN, INPUT);
    pinMode(BATTERY_ADC_PIN, INPUT);
    analogReadResolution(12);
    analogSetPinAttenuation(PRESSURE_ADC_PIN, ADC_11db);
    analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
    if (!beginUi()) Serial.println("[UI] display initialization failed");
    touchAvailable = initializeTouchBus(false);
    Serial.printf("[TOUCH] I2C 0x%02X on GPIO%u/%u: %s\n",
                  TOUCH_I2C_ADDRESS, TOUCH_SDA_PIN, TOUCH_SCL_PIN,
                  touchAvailable ? "ready" : "not found");
    imuMotionWakeAvailable = initializeMotionWake();
    wifiMasterBegin();
    otaServiceBegin();
    bleMasterBegin();
    blePublishBpm(fakeBpm, fakeMaxPressureMmHg, fakeMinPressureMmHg);
    Serial.printf("[SENSOR] analog GPIO%u, nominal 0.2-%.3f V -> 0-%.2f mmHg\n",
                  PRESSURE_ADC_PIN, SENSOR_FULL_SCALE_MV / 1000.0f,
                  SENSOR_FULL_SCALE_MMHG);
    Serial.println("[SENSOR] Keep cuff empty during startup auto-zero");
    // UI responsiveness is the primary requirement: Core 1 is dedicated to
    // the display task at high priority. The sensor blocks between samples,
    // so its short ADC work can safely share Core 0 with the radio stacks.
    xTaskCreatePinnedToCore(sensorTask, "pressure_adc", 4096, nullptr, 3,
                            nullptr, 0);
    xTaskCreatePinnedToCore(uiTask, "dial_30hz", 4096, nullptr, 4,
                            &uiTaskHandle, 1);
}

void loop() {
    otaServiceHandle();
    updateMotionActivity();
    updateTouchGesture();
    BlePhoneCommand phoneCommand{};
    while (bleTakePhoneCommand(phoneCommand)) {
        if (phoneCommand.type == BLE_PHONE_SET_BPM) {
            // Cancel the old range's synthetic display state before publishing
            // new BLE limits. This prevents even one stale 30 Hz UI frame.
            fakePulseMode = false;
            fakePressureOffset = 0.0f;
            fakeUiCancelRevision = fakeUiCancelRevision + 1;
            if (phoneCommand.hasPressureRange) {
                fakeMaxPressureMmHg = phoneCommand.systolicMmHg;
                fakeMinPressureMmHg = phoneCommand.diastolicMmHg;
            }
            // Runtime BLE changes are deliberately RAM-only. NVS/flash writes
            // can suspend instruction-cache access and visibly freeze the UI.
            fakeBpm = phoneCommand.bpm;
            fakeBpmRevision = fakeBpmRevision + 1;
            blePublishBpm(phoneCommand.bpm, fakeMaxPressureMmHg,
                          fakeMinPressureMmHg);
            Serial.printf("[FAKE PULSE] BPM=%u range=%u-%u mmHg active\n",
                          (unsigned)phoneCommand.bpm,
                          (unsigned)fakeMinPressureMmHg,
                          (unsigned)fakeMaxPressureMmHg);
        } else {
            sendPulseTrigger(phoneCommand.pulseType,
                             phoneCommand.targetMask);
        }
    }
    const uint32_t now = millis();
    if (imuMotionWakeAvailable && !standbyEntering &&
        !otaServiceInProgress() &&
        now - lastMotionActivityMs >= STANDBY_TIMEOUT_MS) {
        enterMotionStandby();
    }
    if (Serial && now - lastSerialMs >= 500 &&
        zeroSamples >= ZERO_SAMPLE_COUNT) {
        lastSerialMs = now;
        const int32_t raw = latestRaw;
        const uint32_t milliVolts = latestMilliVolts;
        const float pressure = targetPressure;
        const uint32_t uiAgeMs = now - lastUiFrameMs;
        const uint16_t uiMaxMs = maxUiDrawMs;
        maxUiDrawMs = 0;
        Serial.printf("[SENSOR] adc=%ld delta=%ld mV=%lu zero_mV=%lu mmHg=%.2f sat=%u sps=%u ui_fps=%u ui_age=%lu ui_max=%u bpm=%u range=%u-%u fake=%.2f bat=%umV/%u%% charge_est=%u\n",
                      (long)raw, (long)(raw - zeroRaw),
                      (unsigned long)milliVolts,
                      (unsigned long)zeroMilliVolts, pressure,
                      adcSaturated ? 1U : 0U, measuredSps, measuredUiFps,
                      (unsigned long)uiAgeMs, (unsigned)uiMaxMs,
                      (unsigned)fakeBpm,
                      (unsigned)fakeMinPressureMmHg,
                      (unsigned)fakeMaxPressureMmHg,
                      (double)fakePressureOffset,
                      (unsigned)batteryVoltageMv,
                      (unsigned)batteryPercent,
                      batteryChargingEstimated ? 1U : 0U);
    }
    delay(20);
}
