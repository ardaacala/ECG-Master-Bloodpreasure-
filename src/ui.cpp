#include "ui.h"
#include "config.h"
#include "esp_heap_caps.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>

static Arduino_DataBus *bus = nullptr;
static Arduino_GFX *display = nullptr;
static lv_disp_draw_buf_t drawBuffer;
static lv_color_t *pixels = nullptr;
static lv_obj_t *pulseDot = nullptr;
static lv_obj_t *pulseLabel = nullptr;
static lv_obj_t *statusLabel = nullptr;
static lv_obj_t *bpmLabel = nullptr;
static lv_obj_t *deviceLabels[2] = {nullptr, nullptr};
static bool ready = false;
static bool systemReadyState = false;
static bool systemReadyKnown = false;
static uint8_t displayedConnected = UINT8_MAX;
static uint8_t displayedSynced = UINT8_MAX;
static int64_t pulseOffAtUs = 0;
static bool pulseVisible = false;
static uint32_t lastLvglTickMs = 0;
static int32_t displayedSpreadUs = 0;
static uint32_t displayedRttUs = 0;
static uint32_t displayedJitterUs = 0;
static uint32_t displayedLeadUs = 0;
static bool displayedHasSpread = false;
static uint8_t displayedQuality = 3;
static int8_t displayedRssi = 0;
static uint32_t displayedSent = 0;
static uint32_t displayedAcked = 0;
static uint32_t displayedMissed = 0;
static int64_t displayedCrossLatencyUs = INT64_MIN;

static void writePanelByte(uint8_t value) {
    for (uint8_t i = 0; i < 8; ++i) {
        digitalWrite(PIN_NUM_LCD_DATA0, (value & 0x80) ? HIGH : LOW);
        value <<= 1;
        digitalWrite(PIN_NUM_LCD_PCLK, LOW);
        digitalWrite(PIN_NUM_LCD_PCLK, HIGH);
    }
}

// Waveshare V3 demos read command 0xDA because this board can contain either
// an SH8601 (ID 0x86) or a CO5300 (normally reads 0xFF).
static uint8_t readPanelId() {
    const uint8_t dataPins[] = {PIN_NUM_LCD_DATA0, PIN_NUM_LCD_DATA1, PIN_NUM_LCD_DATA2, PIN_NUM_LCD_DATA3};
    pinMode(PIN_NUM_LCD_CS, OUTPUT);
    pinMode(PIN_NUM_LCD_PCLK, OUTPUT);
    pinMode(PIN_NUM_LCD_RST, OUTPUT);
    for (uint8_t pin : dataPins) pinMode(pin, OUTPUT);

    digitalWrite(PIN_NUM_LCD_CS, HIGH);
    digitalWrite(PIN_NUM_LCD_PCLK, HIGH);
    digitalWrite(PIN_NUM_LCD_RST, HIGH); delay(120);
    digitalWrite(PIN_NUM_LCD_RST, LOW);  delay(120);
    digitalWrite(PIN_NUM_LCD_RST, HIGH); delay(120);

    digitalWrite(PIN_NUM_LCD_CS, LOW);
    writePanelByte(0x03);
    writePanelByte(0x00);
    writePanelByte(0xDA);
    writePanelByte(0x00);

    pinMode(PIN_NUM_LCD_DATA0, INPUT_PULLUP);
    uint8_t id = 0;
    for (uint8_t i = 0; i < 8; ++i) {
        digitalWrite(PIN_NUM_LCD_PCLK, LOW);
        delayMicroseconds(1);
        id = (uint8_t)((id << 1) | digitalRead(PIN_NUM_LCD_DATA0));
        digitalWrite(PIN_NUM_LCD_PCLK, HIGH);
        delayMicroseconds(1);
    }
    digitalWrite(PIN_NUM_LCD_CS, HIGH);
    pinMode(PIN_NUM_LCD_DATA0, OUTPUT);
    return id;
}

static void flushDisplay(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *color) {
    display->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)color, lv_area_get_width(area), lv_area_get_height(area));
    display->flush(); lv_disp_flush_ready(driver);
}

bool uiBegin() {
    const uint8_t panelId = readPanelId();
    const bool isSh8601 = panelId == 0x86;
    Serial.printf("[UI] panel ID=0x%02X, driver=%s\n", panelId, isSh8601 ? "SH8601" : "CO5300");

    bus = new Arduino_ESP32QSPI(PIN_NUM_LCD_CS, PIN_NUM_LCD_PCLK, PIN_NUM_LCD_DATA0, PIN_NUM_LCD_DATA1, PIN_NUM_LCD_DATA2, PIN_NUM_LCD_DATA3, false);
    if (isSh8601) {
        display = new Arduino_SH8601(bus, PIN_NUM_LCD_RST, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    } else {
        display = new Arduino_CO5300(bus, PIN_NUM_LCD_RST, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, 6, 0, 6, 0);
    }
    if (!display || !display->begin(LCD_BUS_SPEED)) {
        Serial.println("[UI] panel begin failed");
        return false;
    }
    // A visible hardware self-test before LVGL starts.
    display->fillScreen(0xF800); display->flush(); delay(350);
    display->fillScreen(0x0000); display->flush(); lv_init();
    lastLvglTickMs = millis();
    pixels = (lv_color_t *)heap_caps_malloc(DISPLAY_WIDTH * 30 * sizeof(lv_color_t), MALLOC_CAP_DMA);
    if (!pixels) {
        Serial.println("[UI] LVGL draw buffer allocation failed");
        return false;
    }
    lv_disp_draw_buf_init(&drawBuffer, pixels, nullptr, DISPLAY_WIDTH * 30);
    static lv_disp_drv_t driver; lv_disp_drv_init(&driver); driver.hor_res = DISPLAY_WIDTH; driver.ver_res = DISPLAY_HEIGHT; driver.flush_cb = flushDisplay; driver.draw_buf = &drawBuffer; lv_disp_drv_register(&driver);
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);
    // Keep all content inside the round panel's safe area. The usable width
    // narrows quickly near the top and bottom edges.
    lv_obj_t *title = lv_label_create(lv_scr_act()); lv_label_set_text(title, "ECG  LIVE"); lv_obj_set_style_text_color(title, lv_color_hex(0x39ff88), 0); lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 25);
    const char *names[] = {"SYS\n120\nmmHg", "DIA\n80\nmmHg", "BPM\n240"}; const uint32_t colors[] = {0xff5470, 0x55aaff, 0xffffff};
    for (int i = 0; i < 3; ++i) { lv_obj_t *label = lv_label_create(lv_scr_act()); lv_label_set_text(label, names[i]); lv_obj_set_style_text_color(label, lv_color_hex(colors[i]), 0); lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0); lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0); lv_obj_align(label, LV_ALIGN_TOP_MID, (i - 1) * 125, 62); if (i == 2) bpmLabel = label; }
    pulseDot = lv_obj_create(lv_scr_act());
    lv_obj_set_size(pulseDot, 105, 105);
    lv_obj_set_style_radius(pulseDot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pulseDot, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_opa(pulseDot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pulseDot, 4, 0);
    lv_obj_set_style_border_color(pulseDot, lv_color_hex(0x505050), 0);
    lv_obj_set_style_shadow_width(pulseDot, 0, 0);
    lv_obj_set_style_pad_all(pulseDot, 0, 0);
    lv_obj_clear_flag(pulseDot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(pulseDot, LV_ALIGN_CENTER, -92, 25);

    pulseLabel = lv_label_create(lv_scr_act());
    lv_obj_set_width(pulseLabel, 205);
    lv_obj_set_style_text_align(pulseLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(pulseLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(pulseLabel, lv_color_hex(0x888888), 0);
    lv_label_set_text(pulseLabel, "TETIK\nBEKLENIYOR");
    lv_obj_align(pulseLabel, LV_ALIGN_CENTER, 72, 25);
    deviceLabels[0] = lv_label_create(lv_scr_act());
    deviceLabels[1] = lv_label_create(lv_scr_act());
    for (lv_obj_t *label : deviceLabels) {
        lv_obj_set_width(label, 170);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_transform_angle(label, 0, 0);
    }
    lv_obj_align(deviceLabels[0], LV_ALIGN_BOTTOM_MID, -95, -64);
    lv_obj_align(deviceLabels[1], LV_ALIGN_BOTTOM_MID, 95, -64);
    statusLabel = lv_label_create(lv_scr_act()); lv_obj_set_width(statusLabel, 390); lv_obj_set_style_text_align(statusLabel, LV_TEXT_ALIGN_CENTER, 0); lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_14, 0); lv_obj_align(statusLabel, LV_ALIGN_BOTTOM_MID, 0, -18);
    ready = true;
    uiSetDeviceCounts(0, 0); uiSetSystemReady(false);
    uiSetCrossTransportLatency(0, false);
    lv_obj_invalidate(lv_scr_act());
    lv_timer_handler();
    Serial.println("[UI] display ready");
    return true;
}
void uiTick() {
    if (!ready) return;
    const uint32_t tickMs = millis();
    if (tickMs - lastLvglTickMs < UI_FRAME_INTERVAL_MS) return;
    lv_tick_inc(tickMs - lastLvglTickMs);
    lastLvglTickMs = tickMs;
    const int64_t nowUs = esp_timer_get_time();
    if (pulseVisible && nowUs >= pulseOffAtUs) {
        pulseVisible = false;
        lv_obj_set_style_bg_color(pulseDot, lv_color_hex(0x202020), 0);
        lv_obj_set_style_border_color(pulseDot, lv_color_hex(0x505050), 0);
        lv_obj_set_style_text_color(pulseLabel, lv_color_hex(0x888888), 0);
        lv_label_set_text(pulseLabel, "TETIK\nBEKLENIYOR");
    }
    lv_timer_handler();
}
void uiSchedulePulse(PulseType type, uint64_t targetMask, int64_t executeAtMasterUs, uint32_t durationUs) {
    if (!ready) return;
    const int64_t nowUs = esp_timer_get_time();
    // 40 ms'lik fiziksel darbe AMOLED'de algilanamayacak kadar kisa ve her
    // on/off cizimi pahali. Gostergeyi en az 300 ms tut; bu surede gelen yeni
    // darbeler yalnizca bitis zamanini uzatsin, tekrar cizim yaptirmasin.
    const int64_t physicalEndUs = executeAtMasterUs + durationUs;
    const int64_t visualEndUs = nowUs + 120000;
    const int64_t endUs = physicalEndUs > visualEndUs ? physicalEndUs : visualEndUs;
    if (endUs > pulseOffAtUs) pulseOffAtUs = endUs;
    if (pulseVisible) return;
    pulseVisible = true;
    const uint32_t color = type == PULSE_LARGE ? 0xff3b30 : 0xffd740;
    lv_obj_set_style_bg_color(pulseDot, lv_color_hex(color), 0);
    lv_obj_set_style_border_color(pulseDot, lv_color_white(), 0);
    lv_obj_set_style_text_color(pulseLabel, lv_color_hex(color), 0);
    if (targetMask == 0) {
        lv_label_set_text(pulseLabel, "TUM LEDLER\nYANDI");
    } else if ((targetMask & (targetMask - 1)) == 0) {
        uint8_t nodeId = 1;
        while ((targetMask >>= 1) != 0) ++nodeId;
        lv_label_set_text_fmt(pulseLabel, "LED %u\nYANDI", nodeId);
    } else {
        uint8_t count = 0;
        uint64_t bits = targetMask;
        while (bits) { count += bits & 1U; bits >>= 1; }
        lv_label_set_text_fmt(pulseLabel, "%u LED\nYANDI", count);
    }
}
void uiSetDeviceCounts(uint8_t connected, uint8_t synced) {
    if (!ready) return;
    if (connected == displayedConnected && synced == displayedSynced) return;
    displayedConnected = connected;
    displayedSynced = synced;
    (void)connected;
    (void)synced;
}

void uiSetNodeStatus(uint8_t nodeId, const char *transport, bool connected,
                     bool synced, uint32_t rttUs) {
    if (!ready || nodeId < 1 || nodeId > 2) return;
    struct CachedNodeStatus {
        bool initialized;
        bool connected;
        bool synced;
        uint32_t rttBucketUs;
    };
    static CachedNodeStatus cache[2] = {};
    CachedNodeStatus &cached = cache[nodeId - 1];
    // RTT metnindeki küçük oynamalar ekranda sürekli pahalı metin çizimine
    // dönüşmesin. 50 ms kovası bağlantı kalitesini göstermeye yeterlidir.
    const uint32_t rttBucketUs = (rttUs / 50000U) * 50000U;
    if (cached.initialized && cached.connected == connected &&
        cached.synced == synced && cached.rttBucketUs == rttBucketUs) return;
    cached = {true, connected, synced, rttBucketUs};
    lv_obj_t *label = deviceLabels[nodeId - 1];
    const char *state = !connected ? "BAGLI DEGIL" : (synced ? "SYNC" : "SYNC BEKLE");
    lv_label_set_text_fmt(label, "SLAVE %u / %s\n%s  RTT %lu.%lums", nodeId,
                          transport, state, (unsigned long)(rttBucketUs / 1000),
                          (unsigned long)((rttBucketUs % 1000) / 100));
    lv_obj_set_style_text_color(label,
        synced ? lv_color_hex(0x39ff88) :
        (connected ? lv_color_hex(0xffd740) : lv_color_hex(0xb0b0b0)), 0);
}

void uiSetCrossTransportLatency(int64_t differenceUs, bool valid) {
    if (!ready || !statusLabel) return;
    const int64_t value = valid ? differenceUs : INT64_MIN;
    if (value == displayedCrossLatencyUs) return;
    displayedCrossLatencyUs = value;
    if (!valid) {
        lv_label_set_text(statusLabel, "WiFi - BLE GECIKME: BEKLENIYOR");
        lv_obj_set_style_text_color(statusLabel, lv_color_hex(0xb0b0b0), 0);
        return;
    }
    const int64_t absoluteUs = llabs(differenceUs);
    lv_label_set_text_fmt(statusLabel, "BLE - WiFi: %s%lld.%03lld ms",
                          differenceUs >= 0 ? "+" : "-",
                          (long long)(absoluteUs / 1000),
                          (long long)(absoluteUs % 1000));
    lv_obj_set_style_text_color(statusLabel,
        absoluteUs <= 5000 ? lv_color_hex(0x39ff88) : lv_color_hex(0xff8c42), 0);
}
static void refreshStatusLabel() {
    if (!ready || !statusLabel) return;
    static const char *qualityNames[] = {"IYI", "ORTA", "ZAYIF", "GUVENSIZ"};
    const uint32_t qualityColors[] = {0x39ff88, 0xffd740, 0xff8c42, 0xff3b30};
    const uint8_t quality = displayedQuality <= 3 ? displayedQuality : 3;
    lv_obj_set_style_text_color(statusLabel, lv_color_hex(qualityColors[quality]), 0);
    if (!systemReadyState) {
        lv_label_set_text_fmt(statusLabel,
                              "%s  RSSI %ddBm  ACK %lu/%lu\nRTT %lu.%lu JIT %lu.%lu  PAY %lums",
                              qualityNames[quality], displayedRssi,
                              (unsigned long)displayedAcked, (unsigned long)displayedSent,
                              (unsigned long)(displayedRttUs / 1000),
                              (unsigned long)((displayedRttUs % 1000) / 100),
                              (unsigned long)(displayedJitterUs / 1000),
                              (unsigned long)((displayedJitterUs % 1000) / 100),
                              (unsigned long)(displayedLeadUs / 1000));
        return;
    }
    if (displayedHasSpread) {
        lv_label_set_text_fmt(statusLabel,
                              "%s RSSI %ddBm ACK %lu/%lu K%lu\nFARK %ld.%03ld RTT %lu.%lu JIT %lu.%lu P%lums",
                              qualityNames[quality], displayedRssi,
                              (unsigned long)displayedAcked, (unsigned long)displayedSent,
                              (unsigned long)displayedMissed,
                              (long)(displayedSpreadUs / 1000),
                              (long)(displayedSpreadUs % 1000),
                              (unsigned long)(displayedRttUs / 1000),
                              (unsigned long)((displayedRttUs % 1000) / 100),
                              (unsigned long)(displayedJitterUs / 1000),
                              (unsigned long)((displayedJitterUs % 1000) / 100),
                              (unsigned long)(displayedLeadUs / 1000));
    } else {
        lv_label_set_text_fmt(statusLabel,
                              "%s RSSI %ddBm ACK %lu/%lu K%lu\nRTT %lu.%lu JIT %lu.%lu PAY %lums",
                              qualityNames[quality], displayedRssi,
                              (unsigned long)displayedAcked, (unsigned long)displayedSent,
                              (unsigned long)displayedMissed,
                              (unsigned long)(displayedRttUs / 1000),
                              (unsigned long)((displayedRttUs % 1000) / 100),
                              (unsigned long)(displayedJitterUs / 1000),
                              (unsigned long)((displayedJitterUs % 1000) / 100),
                              (unsigned long)(displayedLeadUs / 1000));
    }
}

void uiSetReliability(uint8_t quality, int8_t rssiDbm, uint32_t sent,
                      uint32_t acked, uint32_t missed) {
    if (!ready) return;
    if (displayedQuality == quality && displayedRssi == rssiDbm &&
        displayedSent == sent && displayedAcked == acked &&
        displayedMissed == missed) return;
    displayedQuality = quality;
    displayedRssi = rssiDbm;
    displayedSent = sent;
    displayedAcked = acked;
    displayedMissed = missed;
    refreshStatusLabel();
}

void uiSetSystemReady(bool systemReady) {
    if (systemReadyKnown && systemReadyState == systemReady) return;
    systemReadyKnown = true;
    systemReadyState = systemReady;
    // Karma master ekraninda alt satir iki transport arasindaki ACK farkina
    // ayrildi. Hazirlik durumu iki cihaz satirinda ayri ayri gosterilir.
}

void uiSetBpm(uint16_t bpm) {
    if (!ready || !bpmLabel) return;
    static uint16_t displayedBpm = 0;
    if (displayedBpm == bpm) return;
    displayedBpm = bpm;
    lv_label_set_text_fmt(bpmLabel, "BPM\n%u", bpm);
}

void uiSetNetworkMetrics(int32_t spreadUs, uint32_t rttUs, uint32_t jitterUs,
                         uint32_t leadUs, bool hasSpread) {
    if (!ready) return;
    if (displayedSpreadUs == spreadUs && displayedRttUs == rttUs &&
        displayedJitterUs == jitterUs && displayedLeadUs == leadUs &&
        displayedHasSpread == hasSpread) return;
    displayedSpreadUs = spreadUs;
    displayedRttUs = rttUs;
    displayedJitterUs = jitterUs;
    displayedLeadUs = leadUs;
    displayedHasSpread = hasSpread;
    refreshStatusLabel();
}
