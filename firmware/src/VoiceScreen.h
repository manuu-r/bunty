#pragma once
#include <Adafruit_GFX.h>
#include <cstddef>
#include <cstdint>
namespace voiceScreen {
// Labels for the expression preview mode; conversations show only the face.
void heading(Adafruit_GFX &,const char *title,uint8_t opacity=255);
uint16_t ink(uint8_t opacity);
}
