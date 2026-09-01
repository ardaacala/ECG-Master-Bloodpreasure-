#include <Arduino.h>
#include "event_scheduler.h"
#include "config.h"
#include "ui.h"

static ScheduledEvent queue[EVENT_QUEUE_SIZE];
static uint8_t count = 0;

bool schedulerAdd(const ScheduledEvent &event) {
    if (count >= EVENT_QUEUE_SIZE) {
        Serial.println("[SCHED] EVENT QUEUE FULL");
        return false;
    }
    queue[count++] = event;
    return true;
}

void schedulerTick(int64_t nowUs) {
    for (uint8_t index = 0; index < count;) {
        if (queue[index].executeAtMasterUs > nowUs) {
            ++index;
            continue;
        }
        uiSchedulePulse(queue[index].pulseType,
                        queue[index].targetMask,
                        queue[index].executeAtMasterUs,
                        queue[index].durationUs);
        for (uint8_t move = index + 1; move < count; ++move) {
            queue[move - 1] = queue[move];
        }
        --count;
    }
}
