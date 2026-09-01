#pragma once
#include <stdint.h>
#include "protocol.h"

struct ScheduledEvent {
    uint32_t eventId;
    PulseType pulseType;
    uint64_t targetMask;
    int64_t executeAtMasterUs;
    uint32_t durationUs;
};

void schedulerBegin();
bool schedulerAdd(const ScheduledEvent &event);
void schedulerTick(int64_t nowUs);
void schedulerClear();
