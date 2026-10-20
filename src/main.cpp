/* Headless cuff-pressure trigger master for a conditioned 0-40 kPa analog
 * sensor. Sensor output: nominally 0.2-2.7 V at a 3.3 V supply. There is no
 * display or touch controller; status is reported over serial and BLE. */
#include <Arduino.h>
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
// then integrate a CC/CV model instead of mapping 4.2 V to 100%.
// The cell is a Power-Xtra PX103049, 1500 mAh by its label. The gauge shows
// the OCV-curve percentage below, so the integrator has to climb at the rate
// that curve climbs on this cell while charging, not at 500 mA into 1500 mAh
// (32.7 %/h). Its measured charge run, replayed through this estimator, climbs
// 61.3 %/h across the constant-current rows whose rested values settled (rows
// 6-10). The label capacity, the uncalibrated charge current or the generic
// OCV curve is off by that factor; the voltage-map repo leaves which one open
// (its limit 8). On that replay a 1500 mAh integrator lagged the cell's own
// rested reading by up to 27 points; this rate stays within 3, and within 3.5
// on the independent second run except one 5.4-point lag.
constexpr float BATTERY_CHARGE_CC_PERCENT_PER_HOUR = 61.3f;
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
// BLE status LED half periods: fast while searching/syncing, slow when synced.
constexpr uint16_t BLE_LED_SEARCH_HALF_PERIOD_MS = 150;
constexpr uint16_t BLE_LED_SYNCED_HALF_PERIOD_MS = 1000;
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
// The cuff pressure itself is the activity signal. Staying at or below this
// level for the timeout turns the radios off; rising above it wakes the board.
constexpr float STANDBY_PRESSURE_MMHG = 5.0f;
constexpr uint32_t STANDBY_TIMEOUT_MS = 30UL * 60UL * 1000UL;
// While the radios are off, the cuff is checked this often from light sleep.
constexpr uint32_t STANDBY_CHECK_INTERVAL_MS = 250;
// 5 mmHg is only ~35 mV above zero; averaging keeps ADC noise well below it.
constexpr uint8_t STANDBY_SAMPLE_COUNT = 16;
constexpr uint32_t STANDBY_SETTINGS_MAGIC = 0x42505334;

struct StandbyRetainedSettings {
    uint32_t magic;
    uint16_t bpm;
    uint16_t systolicMmHg;
    uint16_t diastolicMmHg;
    // The board wakes with pressure already in the cuff, so the startup
    // auto-zero must not run again; the empty-cuff zero is carried across.
    int32_t zeroRaw;
    uint32_t zeroMilliVolts;
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
uint32_t lastActivityMs = 0;
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

void setBleStatusLed(bool on) {
    // Only the Arduino loop task drives this LED, so this cache needs no lock.
    static int8_t appliedState = -1;
    if (BLE_STATUS_LED_PIN < 0) return;
    const int8_t requestedState = on ? 1 : 0;
    if (requestedState == appliedState) return;
    appliedState = requestedState;
    digitalWrite(BLE_STATUS_LED_PIN,
                 on ? BLE_STATUS_LED_ACTIVE_LEVEL
                    : !BLE_STATUS_LED_ACTIVE_LEVEL);
}

// BLE always runs while loop() does. It is off only before setup() starts it
// and in standby, both of which leave this LED dark.
void updateBleStatusLed(uint32_t nowMs) {
    const uint32_t halfPeriodMs = bleNodeStatus().synced
        ? BLE_LED_SYNCED_HALF_PERIOD_MS
        : BLE_LED_SEARCH_HALF_PERIOD_MS;
    setBleStatusLed((nowMs / halfPeriodMs) % 2 == 0);
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
// data (data/measurements.csv, the first charge run), method and caveats:
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
    // Fitted to the same PX103049 run as the CC rate. In OCV-curve percent the
    // charger leaves constant current near 55 %, where the charge/rest offset
    // starts closing (139 -> 51 mV), and the climb slows to 0.69, 0.41 and
    // 0.14 of the CC rate over 55-66, 66-80 and 80-89 %. Above 89 % the run has
    // no data - it stalled at 4140 mV - so the last measured factor carries on.
    if (percent < 55.0f) return 1.0f;
    if (percent < 66.0f) return 0.69f;
    if (percent < 80.0f) return 0.41f;
    return 0.14f;
}

float advanceBatteryChargeEstimate(float percent, uint32_t elapsedMs) {
    constexpr float ccPercentPerMs =
        BATTERY_CHARGE_CC_PERCENT_PER_HOUR / 3600000.0f;
    return min(100.0f,
               percent + ccPercentPerMs * (float)elapsedMs *
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

// The empty-cuff voltage is measured at startup. Nominal transfer:
// 0.2 V = 0 kPa and 2.7 V = 40 kPa = 300.02 mmHg.
float pressureFromMilliVolts(uint32_t milliVolts) {
    const float usableSpanMv = max(1.0f,
        (float)SENSOR_FULL_SCALE_MV - (float)zeroMilliVolts);
    float p = ((float)milliVolts - (float)zeroMilliVolts) *
              SENSOR_FULL_SCALE_MMHG / usableSpanMv;
    p *= PRESSURE_GAIN_CORRECTION;
    return constrain(p, -20.0f, PRESSURE_MAX_MMHG);
}

void noteActivity() {
    lastActivityMs = millis();
}

void enterPressureStandby() {
    if (standbyEntering) return;

    // The sensor task sees this flag and keeps the local pulse LED dark.
    standbyEntering = true;
    setBleStatusLed(false);

    // Reject all new triggers and allow the last already-transmitted,
    // future-dated event to finish before the radios and CPUs power down.
    const int64_t drainUntilUs = lastTriggerExecuteAtUs == 0
        ? esp_timer_get_time() + 100000
        : lastTriggerExecuteAtUs + LARGE_PULSE_DURATION_US + 20000;
    while (esp_timer_get_time() < drainUntilUs) delay(10);

    // Cuff use during the short drain window cancels standby cleanly.
    if (targetPressure > STANDBY_PRESSURE_MMHG) {
        standbyEntering = false;
        noteActivity();
        return;
    }

    standbyRetainedSettings = {
        STANDBY_SETTINGS_MAGIC,
        (uint16_t)fakeBpm,
        (uint16_t)fakeMaxPressureMmHg,
        (uint16_t)fakeMinPressureMmHg,
        zeroRaw,
        zeroMilliVolts
    };
    Serial.println("[STANDBY] cuff idle for 30 min; radios/triggers off");
    Serial.flush();
    // Deep sleep powers down both cores and the Wi-Fi/BLE radio domains in one
    // step. The timer wake boots into waitForCuffPressure(), which keeps the
    // radios off until the cuff is used again.
    esp_sleep_enable_timer_wakeup((uint64_t)STANDBY_CHECK_INTERVAL_MS * 1000);
    esp_deep_sleep_start();
}

// Called from setup() after a standby wake, before Wi-Fi, BLE, OTA or the
// 500 Hz sensor task exist, so the radios stay off for the whole wait. Light
// sleep keeps RAM; the restored zero and settings survive every check.
void waitForCuffPressure() {
    Serial.printf("[STANDBY] radios off; waiting for cuff > %.0f mmHg\n",
                  STANDBY_PRESSURE_MMHG);
    for (;;) {
        // Discard the first conversion after sleep, then average the rest.
        analogReadMilliVolts(PRESSURE_ADC_PIN);
        uint32_t milliVoltsSum = 0;
        for (uint8_t i = 0; i < STANDBY_SAMPLE_COUNT; ++i) {
            milliVoltsSum += analogReadMilliVolts(PRESSURE_ADC_PIN);
        }
        const float pressure =
            pressureFromMilliVolts(milliVoltsSum / STANDBY_SAMPLE_COUNT);
        if (pressure > STANDBY_PRESSURE_MMHG) {
            Serial.printf("[STANDBY] cuff %.1f mmHg; restoring normal operation\n",
                          pressure);
            return;
        }
        if (Serial.isPlugged()) {
            // Light sleep would drop the USB link to a connected PC, and USB
            // power makes the saving irrelevant there.
            delay(STANDBY_CHECK_INTERVAL_MS);
        } else {
            esp_sleep_enable_timer_wakeup(
                (uint64_t)STANDBY_CHECK_INTERVAL_MS * 1000);
            esp_light_sleep_start();
            // Let the idle task run so the task watchdog stays fed.
            delay(1);
        }
    }
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

    const float cleanPressure = rejectTransientPressureSpike(
        medianPressureSample(pressureFromMilliVolts(milliVolts)));
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
    const bool standbyWake = wakeCause == ESP_SLEEP_WAKEUP_TIMER &&
        standbyRetainedSettings.magic == STANDBY_SETTINGS_MAGIC;
    if (standbyWake) {
        fakeBpm = constrain(standbyRetainedSettings.bpm,
                            (uint16_t)30, (uint16_t)240);
        fakeMaxPressureMmHg = constrain(standbyRetainedSettings.systolicMmHg,
                                        (uint16_t)31, (uint16_t)300);
        fakeMinPressureMmHg = constrain(
            standbyRetainedSettings.diastolicMmHg,
            (uint16_t)1, (uint16_t)(fakeMaxPressureMmHg - 1));
        zeroRaw = standbyRetainedSettings.zeroRaw;
        zeroMilliVolts = standbyRetainedSettings.zeroMilliVolts;
        zeroSamples = ZERO_SAMPLE_COUNT;
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
    if (BLE_STATUS_LED_PIN >= 0) {
        pinMode(BLE_STATUS_LED_PIN, OUTPUT);
        setBleStatusLed(false);
        Serial.printf("[BLE LED] GPIO%d active-%s\n", BLE_STATUS_LED_PIN,
                      BLE_STATUS_LED_ACTIVE_LEVEL == HIGH ? "high" : "low");
    }
    if (standbyWake) waitForCuffPressure();
    wifiMasterBegin();
    otaServiceBegin();
    bleMasterBegin();
    blePublishBpm(fakeBpm, fakeMaxPressureMmHg, fakeMinPressureMmHg);
    Serial.printf("[SENSOR] analog GPIO%u, nominal 0.2-%.3f V -> 0-%.2f mmHg\n",
                  PRESSURE_ADC_PIN, SENSOR_FULL_SCALE_MV / 1000.0f,
                  SENSOR_FULL_SCALE_MMHG);
    if (standbyWake) {
        Serial.printf("[SENSOR] zero %lu mV kept from before standby\n",
                      (unsigned long)zeroMilliVolts);
    } else {
        Serial.println("[SENSOR] Keep cuff empty during startup auto-zero");
    }
    // millis() kept running through any standby wait above; the idle timeout
    // starts now, not at boot.
    noteActivity();
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
    BlePhoneCommand phoneCommand{};
    while (bleTakePhoneCommand(phoneCommand)) {
        // Manual phone triggers work with an empty cuff; keep the radios up.
        noteActivity();
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
    // A running synthetic pulse counts as use even if DIA is set below the
    // standby threshold. An OTA transfer also restarts the idle timeout.
    if (targetPressure > STANDBY_PRESSURE_MMHG || fakePulseMode ||
        otaServiceInProgress() || bleOtaInProgress()) {
        noteActivity();
    }
    const uint32_t now = millis();
    if (now - lastActivityMs >= STANDBY_TIMEOUT_MS) enterPressureStandby();
    updateBleStatusLed(now);
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
