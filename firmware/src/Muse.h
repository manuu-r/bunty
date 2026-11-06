#pragma once
#include <stddef.h>
#include "ServoMotion.h"
namespace muse {
bool begin();
bool setupActive();
bool serviceSetup();
bool paired();
bool connected();
bool wifiConnected();
const char *network();
void disconnect();
void service(ServoMotion *yaw, ServoMotion *pitch, float yawHome, float pitchHome, bool conversationActive);
void resetPairing();
void clearCaption();
void setCaption(const char *text);
void caption(char *out, size_t size);
void setProgress(const char *status, int percent = -1);
void clearProgress();
bool getProgress(char *status, size_t size, int &percent);
}
