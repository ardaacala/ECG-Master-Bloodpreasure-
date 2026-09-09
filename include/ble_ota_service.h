#pragma once

#include <stdint.h>

class NimBLEServer;

bool bleOtaServiceBegin(NimBLEServer *server);
void bleOtaServiceOnDisconnect(uint16_t connectionHandle);
bool bleOtaInProgress();
