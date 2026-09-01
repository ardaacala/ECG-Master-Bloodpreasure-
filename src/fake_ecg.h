#pragma once
#include <stdint.h>
#include "protocol.h"
struct EcgEvent {
    PulseType type;
    int64_t atUs;
    uint64_t targetMask;
    uint16_t bpm;
};
bool fakeEcgNextEvent(int64_t nowUs, EcgEvent &event);
