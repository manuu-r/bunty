#pragma once
#include "Arduino.h"
#include <string>
using String = std::string;
#define PROGMEM
class __FlashStringHelper;
inline float radians(float degrees) { return degrees * 0.017453292519943295f; }
