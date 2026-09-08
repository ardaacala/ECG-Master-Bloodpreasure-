#include "ui.h"

// The combined blood-pressure UI draws connection state directly on its
// Arduino_GFX canvas. These keep the reused transport module API compatible.
bool uiBegin() { return true; }
void uiTick() {}
void uiSchedulePulse(PulseType, uint64_t, int64_t, uint32_t) {}
void uiSetDeviceCounts(uint8_t, uint8_t) {}
void uiSetSystemReady(bool) {}
void uiSetBpm(uint16_t) {}
void uiSetNetworkMetrics(int32_t, uint32_t, uint32_t, uint32_t, bool) {}
void uiSetReliability(uint8_t, int8_t, uint32_t, uint32_t, uint32_t) {}
void uiSetNodeStatus(uint8_t, const char *, bool, bool, uint32_t) {}
void uiSetCrossTransportLatency(int64_t, bool) {}
