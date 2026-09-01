#include "fake_ecg.h"

static const uint16_t BPM_STAGES[] = {240, 120, 72, 50};
static const uint64_t SEQUENTIAL_TARGETS[] = {
    maskForDevice(1),             // Trigger 1: yalniz DEVICE_ID 1
    maskForDevice(1),             // Trigger 2: yalniz DEVICE_ID 1
    maskForDevice(2),             // Trigger 3: yalniz DEVICE_ID 2
    maskForDevice(2),             // Trigger 4: yalniz DEVICE_ID 2
    0                             // Trigger 5: tum bagli slave'ler senkron
};

static bool initialized = false;
static int64_t nextTriggerUs = 0;
static uint8_t bpmStage = 0;
static uint8_t triggerInStage = 0;
static bool synchronizedCycle = true;

bool fakeEcgNextEvent(int64_t nowUs, EcgEvent &event) {
    if (!initialized) {
        nextTriggerUs = nowUs + 60000000LL / BPM_STAGES[0];
        initialized = true;
    }
    if (nowUs < nextTriggerUs) return false;

    // Ilk tam 240->120->72->50 dongusunde her trigger broadcast'tir.
    // Ikinci tam dongude 1,1,2,2,broadcast hedef dizisi kullanilir.
    const uint64_t targetMask = synchronizedCycle
                                    ? 0
                                    : SEQUENTIAL_TARGETS[triggerInStage];
    event = {PULSE_LARGE, nextTriggerUs, targetMask,
             BPM_STAGES[bpmStage]};

    ++triggerInStage;
    if (triggerInStage == 5) {
        triggerInStage = 0;
        ++bpmStage;
        if (bpmStage == 4) {
            bpmStage = 0;
            synchronizedCycle = !synchronizedCycle;
        }
    }
    // Besinci triggerden sonraki aralik yeni BPM'in periyodudur.
    nextTriggerUs += 60000000LL / BPM_STAGES[bpmStage];
    return true;
}
