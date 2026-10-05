#pragma once
#include "Runtime.h"
#include <stdarg.h>
#include <stdio.h>

// Arduino Print::printf mallocs for messages over 63 bytes. Diagnostics must
// not fragment the heap they are measuring. Bound each record on the task's
// internal stack; oversized cloud text is deliberately truncated in logs.
inline void logPrintf(const char *format, ...) __attribute__((format(printf, 1, 2)));
inline void logPrintf(const char *format, ...) {
  char message[256];
  va_list args;
  va_start(args, format);
  const int length = vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  if (length <= 0) return;
  if (length >= int(sizeof(message))) message[sizeof(message) - 2] = '\n';
  Serial.write(reinterpret_cast<const uint8_t *>(message),
               min(size_t(length), sizeof(message) - 1));
}
