#pragma once
#include <stdint.h>
#include "protocol.h"

bool uiBegin();
void uiTick();
void uiSchedulePulse(PulseType type, uint64_t targetMask, int64_t executeAtMasterUs, uint32_t durationUs);
void uiSetDeviceCounts(uint8_t connected, uint8_t synced);
void uiSetSystemReady(bool ready);
void uiSetBpm(uint16_t bpm);
void uiSetNetworkMetrics(int32_t spreadUs, uint32_t rttUs, uint32_t jitterUs,
                         uint32_t leadUs, bool hasSpread);
void uiSetReliability(uint8_t quality, int8_t rssiDbm, uint32_t sent,
                      uint32_t acked, uint32_t missed);
void uiSetNodeStatus(uint8_t nodeId, const char *transport, bool connected,
                     bool synced, uint32_t rttUs);
void uiSetCrossTransportLatency(int64_t differenceUs, bool valid);
