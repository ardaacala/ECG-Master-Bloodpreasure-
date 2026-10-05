/* Headless cuff-pressure trigger master for a conditioned 0-40 kPa analog
 * sensor. Sensor output: nominally 0.2-2.7 V at a 3.3 V supply. There is no
 * display or touch controller; status is reported over serial and BLE. */
#include <Arduino.h>
#include <Wire.h>
#include "esp_sleep.h"
#include "esp_timer.h"
#include <Preferences.h>
#include "config.h"
#include "protocol.h"
#include "wifi_master.h"
#include "ble_master.h"
#include "ble_ota_service.h"
#include "ota_service.h"

// The battery divider feeds GPIO4 and the external conditioned pressure sensor
// feeds GPIO2. Both are ADC1 channels, which keep working while Wi-Fi is up.
constexpr uint8_t BATTERY_ADC_PIN = 4;
constexpr uint8_t PRESSURE_ADC_PIN = 2;
// ADC1 channel 3 (GPIO4) sees one third of the cell voltage, as on the
// original Waveshare board; change this together with the PCB divider.
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
// Above this charge terminal voltage the ETA6098 is deep in constant-voltage
// taper: the voltage is pinned by the charger and no longer tracks state of
// charge, so the measured charge/rest map stops being informative and the CC/CV
// integrator alone carries the last stretch to 100%. It sits one map step below
// the 4140 mV plateau the reference run got stuck at, which keeps margin for the
// +/-20-30 mV absolute ADC offset that run documents.
constexpr uint16_t BATTERY_CHARGE_MAP_RELEASE_MV = 4100;
// Second, slower route into charge mode, for the case the step detector cannot
// see: a board that boots with USB already attached never produces a step, so
// there is nothing to catch. Constant-current charging lifts this pack 30-45 mV
// every five minutes and keeps doing it for hours, while a cell merely relaxing
// after a load drop recovers within a minute or two and then flattens out.
// Requiring two consecutive qualifying windows is what separates the two.
constexpr uint32_t BATTERY_RISE_WINDOW_MS = 300000;
constexpr uint16_t BATTERY_RISE_WINDOW_MIN_MV = 20;
constexpr uint8_t BATTERY_RISE_WINDOWS_REQUIRED = 2;
constexpr uint16_t BATTERY_RISE_RESET_MV = 10;
// A downward correction walks to the map ceiling instead of snapping to it.
// Only the sustained-rise path can open a session above the ceiling, and a
// gauge that jumps backwards while the cable is in reads as a fault.
constexpr float BATTERY_CHARGE_CORRECTION_STEP_PERCENT = 1.0f;
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
constexpr float PRESSURE_MAX_MMHG = 300.0f;
constexpr uint16_t ADC_SAMPLE_HZ = 500;
constexpr uint32_t ADC_SAMPLE_PERIOD_US = 1000000UL / ADC_SAMPLE_HZ;
constexpr uint8_t PRESSURE_MEDIAN_WINDOW = 5;
constexpr float PRESSURE_SPIKE_STEP_MMHG = 2.5f;
constexpr float PRESSURE_SPIKE_RETURN_MMHG = 1.0f;
constexpr uint8_t PRESSURE_SPIKE_CONFIRM_SAMPLES = 8;
constexpr uint16_t PULSE_LED_ON_MS = 150;
// Ignore brief ADC excursions around the configured pressure boundaries.
// The beat timeline keeps running during this window, so boundary noise
// cannot turn 30 BPM into rapid restarts or make 200 BPM appear very slow.
constexpr uint16_t FAKE_RANGE_EXIT_DEBOUNCE_MS = 300;
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
// lead for every event so the physical/LED cadence remains exactly BPM-based.
constexpr uint32_t FIXED_TRIGGER_LEAD_US = 600000;
// A cuff oscillation can contain a reflected/dicrotic second positive lobe.
// Keep the real detector refractory long enough not to count that lobe as a
// second heartbeat. 500 ms still supports heart rates up to 120 BPM.
constexpr uint16_t WARMUP_SAMPLE_COUNT = 125;
constexpr uint16_t ZERO_SAMPLE_COUNT = 250;
// QMI8658 I2C bus. It is accessed only from the Arduino loop on Core 0.
constexpr uint8_t IMU_SDA_PIN = 47, IMU_SCL_PIN = 48;
// QMI8658 INT1 must reach GPIO8, an RTC GPIO, so it can wake deep sleep.
// Wake-on-Motion uses the accelerometer's low-power engine; the gyroscope
// must stay disabled for that mode according to the sensor data sheet.
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
constexpr uint32_t STANDBY_SETTINGS_MAGIC = 0x42505333;

struct StandbyRetainedSettings {
    uint32_t magic;
    uint16_t bpm;
    uint16_t systolicMmHg;
    uint16_t diastolicMmHg;
};

RTC_DATA_ATTR StandbyRetainedSettings standbyRetainedSettings = {};

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
volatile uint16_t fakeBpm = DEFAULT_FAKE_BPM;
volatile uint16_t fakeMaxPressureMmHg = DEFAULT_FAKE_MAX_PRESSURE_MMHG;
volatile uint16_t fakeMinPressureMmHg = DEFAULT_FAKE_MIN_PRESSURE_MMHG;
volatile uint32_t fakeBpmRevision = 0;
// True while the synthetic beat is active: DIA < measured mmHg < SYS.
volatile bool fakePulseMode = false;
volatile uint16_t measuredSps = 0;
volatile uint16_t batteryVoltageMv = 0;
volatile uint8_t batteryPercent = 0;
volatile bool batteryReadingValid = false;
volatile bool batteryChargingEstimated = false;
bool imuMotionWakeAvailable = false;
uint8_t qmi8658Address = 0;
uint32_t lastMotionActivityMs = 0;
volatile bool standbyEntering = false;
uint32_t lastSerialMs = 0;
uint32_t networkEventId = 0;
int64_t lastTriggerExecuteAtUs = 0;
portMUX_TYPE triggerMux = portMUX_INITIALIZER_UNLOCKED;

void setPulseLed(bool on) {
    // Only the sensor task drives the LED, so this cache needs no lock.
    static int8_t appliedState = -1;
    if (PULSE_LED_PIN < 0) return;
    const int8_t requestedState = on ? 1 : 0;
    if (requestedState == appliedState) return;
    appliedState = requestedState;
    digitalWrite(PULSE_LED_PIN,
                 on ? PULSE_LED_ACTIVE_LEVEL : !PULSE_LED_ACTIVE_LEVEL);
}

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

// Measured on a Power-Xtra PX103049 (3.7 V / 1500 mAh Li-Po), the cell this
// board ships with: each pair is the terminal voltage read while charging and
// the same cell's voltage 60 s after the charge path was switched off. Source
// data, method and caveats:
// https://github.com/ardaacala/powerxtra-px103049-voltage-map
// While the cable is in, IR drop and polarization lift BAT_ADC well above open
// circuit - a steady ~140 mV through constant current, closing to ~51 mV as the
// charger tapers. Feeding that raw voltage into the OCV curve is what reports a
// half-empty cell as nearly full; mapping it back to its rested equivalent
// first removes the bias.
struct BatteryChargeRestPoint { float chargingMv; float restedMv; };
constexpr BatteryChargeRestPoint BATTERY_CHARGE_REST_MAP[] = {
    {3426.44f, 3260.81f}, {3482.88f, 3353.94f}, {3558.56f, 3415.94f},
    {3606.88f, 3467.69f}, {3663.88f, 3519.69f}, {3721.75f, 3579.50f},
    {3784.12f, 3645.56f}, {3841.00f, 3702.88f}, {3901.62f, 3762.25f},
    {3961.38f, 3834.31f}, {4020.25f, 3907.44f}, {4081.50f, 3999.75f},
    {4140.12f, 4089.25f}
};
constexpr size_t BATTERY_CHARGE_REST_MAP_COUNT =
    sizeof(BATTERY_CHARGE_REST_MAP) / sizeof(BATTERY_CHARGE_REST_MAP[0]);

// Piecewise-linear between measured pairs, never extrapolated: outside the
// 3426-4140 mV span the reference run has no data, so the caller keeps using
// the CC/CV estimate on its own.
bool batteryRestedMvFromChargingMv(uint16_t chargingMv, float *restedMvOut) {
    const float mv = (float)chargingMv;
    if (mv < BATTERY_CHARGE_REST_MAP[0].chargingMv) return false;
    if (mv > BATTERY_CHARGE_REST_MAP[BATTERY_CHARGE_REST_MAP_COUNT - 1]
                 .chargingMv) {
        return false;
    }
    for (size_t i = 1; i < BATTERY_CHARGE_REST_MAP_COUNT; ++i) {
        const BatteryChargeRestPoint &low = BATTERY_CHARGE_REST_MAP[i - 1];
        const BatteryChargeRestPoint &high = BATTERY_CHARGE_REST_MAP[i];
        if (mv <= high.chargingMv) {
            const float spanMv = high.chargingMv - low.chargingMv;
            const float ratio = (mv - low.chargingMv) / spanMv;
            *restedMvOut = low.restedMv + ratio * (high.restedMv - low.restedMv);
            return true;
        }
    }
    return false;
}

// Highest percentage the charge-time integrator is allowed to report for a
// given charge voltage. This is a ceiling, not the shown value: the reference
// run rested each point for only 60 s, which leaves its 3.35-3.52 V rested
// numbers reading somewhat high, and it is a single cell at room temperature.
// The CC/CV model still draws the smooth curve underneath; the map only stops
// it from running ahead of what the cell's own voltage supports.
bool batteryChargingPercentCeiling(uint16_t chargingMv, float *ceilingOut) {
    if (chargingMv > BATTERY_CHARGE_MAP_RELEASE_MV) return false;
    float restedMv = 0.0f;
    if (!batteryRestedMvFromChargingMv(chargingMv, &restedMv)) return false;
    const uint16_t restedRounded =
        (uint16_t)constrain(lroundf(restedMv), 0L, 5000L);
    *ceilingOut = (float)batteryPercentFromMillivolts(restedRounded);
    return true;
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
    // pressure stream, which showed up as a periodic sampling freeze.
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
    static float chargingFloorPercent = 0.0f;
    static uint16_t riseWindowStartMv = 0;
    static uint16_t riseWindowPeakMv = 0;
    static uint32_t riseWindowStartedMs = 0;
    static bool riseWindowActive = false;
    static uint8_t sustainedRiseWindows = 0;
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
        chargingFloorPercent = 0.0f;
        sustainedRiseWindows = 0;
        riseWindowActive = false;
        batteryChargingEstimated = false;
    } else if (charging) {
        chargingPeakMv = max(chargingPeakMv, measuredMv);
        estimatedBatteryPercent = advanceBatteryChargeEstimate(
            estimatedBatteryPercent, elapsedMs);
        float ceilingPercent = 0.0f;
        if (batteryChargingPercentCeiling(publishedMv, &ceilingPercent)) {
            // The floor keeps the indicator from stepping backwards on a
            // momentary ceiling dip; the ceiling caps further rise. A session
            // opened above the ceiling walks down to it a step per update
            // rather than snapping, and the floor never latches a value the
            // ceiling has not yet approved.
            const float lowerBound = max(ceilingPercent, chargingFloorPercent);
            if (estimatedBatteryPercent > lowerBound) {
                estimatedBatteryPercent =
                    max(lowerBound,
                        estimatedBatteryPercent -
                            BATTERY_CHARGE_CORRECTION_STEP_PERCENT);
            } else {
                estimatedBatteryPercent =
                    max(estimatedBatteryPercent, chargingFloorPercent);
            }
            chargingFloorPercent =
                max(chargingFloorPercent,
                    min(estimatedBatteryPercent, ceilingPercent));
        } else {
            chargingFloorPercent =
                max(chargingFloorPercent, estimatedBatteryPercent);
        }
        batteryChargingEstimated = true;
    } else {
        // Only an uninterrupted climb qualifies. Measuring the fall against
        // the running peak rather than the window start is what rejects the
        // rise-then-plateau shape of a cell relaxing after a load drop: a net
        // gain across the window is not enough, the climb has to still be
        // going. The filtered voltage is used so ADC noise cannot chop a
        // window up, and a window is judged only once it has fully elapsed.
        if (!riseWindowActive ||
            publishedMv + BATTERY_RISE_RESET_MV < riseWindowPeakMv) {
            riseWindowActive = true;
            riseWindowStartMv = publishedMv;
            riseWindowPeakMv = publishedMv;
            riseWindowStartedMs = nowMs;
            sustainedRiseWindows = 0;
        } else {
            riseWindowPeakMv = max(riseWindowPeakMv, publishedMv);
            if (nowMs - riseWindowStartedMs >= BATTERY_RISE_WINDOW_MS) {
                if (publishedMv >=
                    riseWindowStartMv + BATTERY_RISE_WINDOW_MIN_MV) {
                    if (sustainedRiseWindows < UINT8_MAX) {
                        ++sustainedRiseWindows;
                    }
                } else {
                    sustainedRiseWindows = 0;
                }
                riseWindowStartMv = publishedMv;
                riseWindowPeakMv = publishedMv;
                riseWindowStartedMs = nowMs;
            }
        }

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
            sustainedRiseWindows = 0;
            riseWindowActive = false;
            // Include the short confirmation interval without ever using the
            // charge-biased voltage as a percentage.
            estimatedBatteryPercent = advanceBatteryChargeEstimate(
                preChargeBatteryPercent, nowMs - chargeCandidateStartedMs);
            // The last battery-only reading is the trustworthy one, so it
            // anchors the session floor the charge map may not pull under.
            chargingFloorPercent = preChargeBatteryPercent;
            float entryCeilingPercent = 0.0f;
            if (batteryChargingPercentCeiling(publishedMv,
                                              &entryCeilingPercent)) {
                estimatedBatteryPercent =
                    max(min(estimatedBatteryPercent, entryCeilingPercent),
                        chargingFloorPercent);
            }
            batteryChargingEstimated = true;
        } else if (chargeCandidate) {
            // Hold the last battery-only result during the 15 s confirmation.
            estimatedBatteryPercent = preChargeBatteryPercent;
            batteryChargingEstimated = false;
        } else if (sustainedRiseWindows >= BATTERY_RISE_WINDOWS_REQUIRED) {
            // No trustworthy pre-charge reading exists on this path - it
            // exists precisely because the cable was already in when the board
            // booted - so the charge/rest map supplies the anchor instead.
            // The floor stays at zero so that map is free to correct a
            // percentage the raw charge voltage inflated; the charging branch
            // walks it down rather than snapping.
            charging = true;
            chargingPeakMv = measuredMv;
            chargingFloorPercent = 0.0f;
            sustainedRiseWindows = 0;
            riseWindowActive = false;
            batteryChargingEstimated = true;
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
    blePublishBatteryLevel(batteryPercent, batteryReadingValid);
    previousMeasuredBatteryMv = measuredMv;
    previousUpdateMs = nowMs;
}

bool sendPulseTrigger(PulseType pulseType = PULSE_LARGE,
                      uint64_t targetMask = 0,
                      int64_t *executeAtResultUs = nullptr,
                      uint32_t *eventIdResult = nullptr,
                      bool reportToPhone = false) {
    if (standbyEntering || otaServiceInProgress() || bleOtaInProgress()) {
        return false;
    }
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
    // V3 physical outputs are generated locally by each slave from the
    // retained TriggerStatePacket. This function now schedules only the
    // master's synchronized local pulse LED and never floods transports per
    // beat.
    if (ENABLE_VERBOSE_TIMING_LOGS) {
        Serial.printf("[LOCAL PULSE] #%lu at=%lld target=0x%llx type=%u\n",
                      (unsigned long)eventId, (long long)executeAtUs,
                      (unsigned long long)targetMask, (unsigned)pulseType);
    }
    if (reportToPhone) blePublishPhoneResult(eventId, false, false);
    return true;
}

void updateTriggerState(float pressureMmHg, bool saturated) {
    static bool initialized = false;
    static uint16_t publishedBpm = 0;
    static PressureZone publishedZone = PRESSURE_BELOW;
    static uint32_t revision = 0;

    const uint16_t bpm = constrain((uint16_t)fakeBpm,
                                   (uint16_t)30, (uint16_t)240);
    PressureZone zone = PRESSURE_INSIDE;
    if (saturated || pressureMmHg >= (float)fakeMaxPressureMmHg) {
        zone = PRESSURE_ABOVE;
    } else if (pressureMmHg <= (float)fakeMinPressureMmHg) {
        zone = PRESSURE_BELOW;
    }
    if (initialized && bpm == publishedBpm && zone == publishedZone) return;

    initialized = true;
    publishedBpm = bpm;
    publishedZone = zone;
    ++revision;
    const int64_t effectiveAtUs = esp_timer_get_time() + FIXED_TRIGGER_LEAD_US;
    TriggerStatePacket packet{MSG_TRIGGER_STATE, 3, WIFI_NODE_ID, revision,
                              bpm, (uint8_t)zone, 0, effectiveAtUs};
    const bool wifiQueued = wifiPublishTriggerState(packet);
    packet.receiverDeviceId = BLE_NODE_ID;
    const bool bleQueued = blePublishTriggerState(packet);
    Serial.printf("[STATE] rev=%lu bpm=%u zone=%u wifi=%u ble=%u at=%lld\n",
                  (unsigned long)revision, (unsigned)bpm, (unsigned)zone,
                  wifiQueued ? 1U : 0U, bleQueued ? 1U : 0U,
                  (long long)effectiveAtUs);
}

bool qmi8658ReadRegisterAt(uint8_t address, uint8_t reg, uint8_t &value) {
    Wire.beginTransmission(address);
    Wire.write(reg);
    // QMI8658 retains the register pointer across STOP. Avoiding a repeated
    // start here keeps the shared Arduino I2C peripheral out of the sporadic
    // INVALID_STATE seen when touch and IMU no-stop reads were interleaved on
    // the original display board.
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
    // the gyro, sensor task and radio tasks are not touched by this polling path.
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

    // The sensor task sees this flag and keeps the local pulse LED dark.
    standbyEntering = true;
    fakePulseMode = false;
    standbyRetainedSettings = {
        STANDBY_SETTINGS_MAGIC,
        (uint16_t)fakeBpm,
        (uint16_t)fakeMaxPressureMmHg,
        (uint16_t)fakeMinPressureMmHg
    };

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

    // Rearm only after the drain window, then wait out the configured eight
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

    Serial.println("[STANDBY] no motion for 5 min; radios/triggers off");
    Serial.flush();
    // Deep sleep stops both cores and the Wi-Fi/BLE radio domains. QMI8658
    // remains powered in accelerometer-only WoM mode and raises GPIO8 on motion.
    esp_deep_sleep_start();
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
    constexpr uint8_t LOCAL_PULSE_QUEUE_SIZE = 8;
    static uint32_t nextBeatMs = 0;
    static uint32_t outOfRangeSinceMs = 0;
    static uint32_t appliedBpmRevision = 0;
    static int64_t scheduledPulseUs[LOCAL_PULSE_QUEUE_SIZE] = {};
    static uint8_t pulseHead = 0;
    static uint8_t pulseCount = 0;
    static int64_t pulseLedUntilUs = 0;
    static bool rangeActive = false;

    const auto clearScheduledPulses = [&]() {
        pulseHead = 0;
        pulseCount = 0;
        pulseLedUntilUs = 0;
        setPulseLed(false);
    };

    if (otaServiceInProgress() || bleOtaInProgress()) {
        rangeActive = false;
        nextBeatMs = 0;
        outOfRangeSinceMs = 0;
        clearScheduledPulses();
        fakePulseMode = false;
        return;
    }

    if (appliedBpmRevision != fakeBpmRevision) {
        appliedBpmRevision = fakeBpmRevision;
        nextBeatMs = 0;
        outOfRangeSinceMs = 0;
        rangeActive = false;
        clearScheduledPulses();
    }

    // Synthetic BPM exists only strictly between the BLE diastolic and
    // systolic values: DIA < measured mmHg < SYS.
    const bool rawPressureInRange = !saturated &&
        realPressure > (float)fakeMinPressureMmHg &&
        realPressure < (float)fakeMaxPressureMmHg;
    if (!rangeActive) {
        if (!rawPressureInRange) {
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
            // Clearing also cancels LED events queued for the old valid range.
            rangeActive = false;
            outOfRangeSinceMs = 0;
            nextBeatMs = 0;
            clearScheduledPulses();
            fakePulseMode = false;
            return;
        }
    }

    // Keep the 300 ms debounce only for the beat timeline. The local LED
    // follows the strict pressure bounds immediately, so a pulse cannot remain
    // visible at/below DIA or above SYS while the cadence state waits to see
    // whether this was merely boundary noise.
    fakePulseMode = rawPressureInRange;
    if (!rawPressureInRange) pulseLedUntilUs = 0;

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
            pulseCount < LOCAL_PULSE_QUEUE_SIZE &&
            sendPulseTrigger(PULSE_LARGE, 0, &executeAtUs, &eventId)) {
            const uint8_t tail = (pulseHead + pulseCount) %
                                 LOCAL_PULSE_QUEUE_SIZE;
            scheduledPulseUs[tail] = executeAtUs;
            ++pulseCount;
        }
    }

    // Drain acknowledgements; all outputs and the local LED still start from
    // the same master executeAt timestamp. Waiting for the first ACK here
    // would make the LED follow one transport and de-sync from the other.
    WifiLedAck wifiAck{};
    while (wifiTakeLedAck(wifiAck)) {}
    BleLedAck bleAck{};
    while (bleTakeLedAck(bleAck)) {}

    while (pulseCount > 0 && nowUs >= scheduledPulseUs[pulseHead]) {
        const int64_t duePulseUs = scheduledPulseUs[pulseHead];
        pulseHead = (pulseHead + 1) % LOCAL_PULSE_QUEUE_SIZE;
        --pulseCount;
        // An event that falls due while out of range is consumed, so it
        // cannot flash late when pressure re-enters the range.
        if (rawPressureInRange) {
            pulseLedUntilUs = duePulseUs + (int64_t)PULSE_LED_ON_MS * 1000;
        }
    }

    // The 500 Hz sensor task gives the LED 2 ms timing resolution.
    setPulseLed(!standbyEntering && rawPressureInRange &&
                nowUs < pulseLedUntilUs);
}

float medianPressureSample(float sample) {
    // Fixed-size stack/ring buffers only: at 500 Hz this is at most ten tiny
    // comparisons per sample and never allocates.
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
        // complete excursion instead of letting even one trigger decision
        // see it.
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
        // the median window its added latency is about 20 ms.
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
    p = constrain(p, -20.0f, PRESSURE_MAX_MMHG);
    const float cleanPressure = rejectTransientPressureSpike(
        medianPressureSample(p));
    latestRaw = raw;
    latestMilliVolts = milliVolts;
    adcSaturated = raw >= ADC_RESOLUTION_MAX - 3 || milliVolts >= 3050;
    // Trigger-state and synthetic range decisions share the cleaned physical
    // pressure.
    targetPressure = max(0.0f, cleanPressure);
    const uint32_t nowMs = millis();
    updateTriggerState(targetPressure, adcSaturated);
    updateFakePulse(targetPressure, adcSaturated, nowMs);
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
        // CPU time for the radio stacks.
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
    if (PULSE_LED_PIN >= 0) {
        pinMode(PULSE_LED_PIN, OUTPUT);
        setPulseLed(false);
        Serial.printf("[PULSE LED] GPIO%d active-%s\n", PULSE_LED_PIN,
                      PULSE_LED_ACTIVE_LEVEL == HIGH ? "high" : "low");
    }
    const bool imuBusReady = Wire.begin(IMU_SDA_PIN, IMU_SCL_PIN, 400000);
    if (imuBusReady) {
        Wire.setTimeOut(10);
    } else {
        Serial.printf("[STANDBY] I2C GPIO%u/%u init failed; auto-standby disabled\n",
                      IMU_SDA_PIN, IMU_SCL_PIN);
    }
    imuMotionWakeAvailable = imuBusReady && initializeMotionWake();
    wifiMasterBegin();
    otaServiceBegin();
    bleMasterBegin();
    blePublishBpm(fakeBpm, fakeMaxPressureMmHg, fakeMinPressureMmHg);
    Serial.printf("[SENSOR] analog GPIO%u, nominal 0.2-%.3f V -> 0-%.2f mmHg\n",
                  PRESSURE_ADC_PIN, SENSOR_FULL_SCALE_MV / 1000.0f,
                  SENSOR_FULL_SCALE_MMHG);
    Serial.println("[SENSOR] Keep cuff empty during startup auto-zero");
    // The sensor blocks between samples, so its short ADC work can safely
    // share Core 0 with the radio stacks.
    xTaskCreatePinnedToCore(sensorTask, "pressure_adc", 4096, nullptr, 3,
                            nullptr, 0);
}

const char *linkStateLabel(bool connected, bool synced) {
    return synced ? "OK" : (connected ? "SYNC" : "OFF");
}

void loop() {
    if (!bleOtaInProgress()) otaServiceHandle();
    updateMotionActivity();
    BlePhoneCommand phoneCommand{};
    while (bleTakePhoneCommand(phoneCommand)) {
        if (phoneCommand.type == BLE_PHONE_SET_BPM) {
            if (phoneCommand.hasPressureRange) {
                fakeMaxPressureMmHg = phoneCommand.systolicMmHg;
                fakeMinPressureMmHg = phoneCommand.diastolicMmHg;
            }
            // Runtime BLE changes are deliberately RAM-only. NVS/flash writes
            // can suspend instruction-cache access and stall 500 Hz sampling.
            // The revision bump also cancels LED events of the old range.
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
                             phoneCommand.targetMask, nullptr, nullptr, true);
        }
    }
    const uint32_t now = millis();
    if (imuMotionWakeAvailable && !standbyEntering &&
        !otaServiceInProgress() && !bleOtaInProgress() &&
        now - lastMotionActivityMs >= STANDBY_TIMEOUT_MS) {
        enterMotionStandby();
    }
    if (Serial && now - lastSerialMs >= 500 &&
        zeroSamples >= ZERO_SAMPLE_COUNT) {
        lastSerialMs = now;
        const int32_t raw = latestRaw;
        const uint32_t milliVolts = latestMilliVolts;
        const float pressure = targetPressure;
        // Without a display, this line also carries the former on-screen
        // W:/B: connection strip.
        const WifiNodeStatus wifi = wifiNodeStatus(WIFI_NODE_ID);
        const BleNodeStatus ble = bleNodeStatus();
        Serial.printf("[SENSOR] adc=%ld delta=%ld mV=%lu zero_mV=%lu mmHg=%.2f sat=%u sps=%u bpm=%u range=%u-%u pulse=%u wifi=%s ble=%s bat=%umV/%u%% charge_est=%u\n",
                      (long)raw, (long)(raw - zeroRaw),
                      (unsigned long)milliVolts,
                      (unsigned long)zeroMilliVolts, pressure,
                      adcSaturated ? 1U : 0U, measuredSps,
                      (unsigned)fakeBpm,
                      (unsigned)fakeMinPressureMmHg,
                      (unsigned)fakeMaxPressureMmHg,
                      fakePulseMode ? 1U : 0U,
                      linkStateLabel(wifi.connected, wifi.synced),
                      linkStateLabel(ble.connected, ble.synced),
                      (unsigned)batteryVoltageMv,
                      (unsigned)batteryPercent,
                      batteryChargingEstimated ? 1U : 0U);
    }
    delay(20);
}
