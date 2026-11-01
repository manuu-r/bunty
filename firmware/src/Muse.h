#pragma once
#include <stddef.h>
#include "ServoMotion.h"
namespace muse {
bool begin();
bool setupActive();
bool serviceSetup();
bool paired();
bool connected();
const char *network();
void disconnect();
void service(ServoMotion *yaw, ServoMotion *pitch, float yawHome, float pitchHome, bool conversationActive);
void resetPairing();
void clearCaption();
void caption(char *out, size_t size);
}
