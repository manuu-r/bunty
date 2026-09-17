#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Reassembles one text payload split across IDF websocket data events. IDF
// 4.4 does not expose FIN, so RFC text continuation frames are rejected.
// The gateway uses whole text messages; MP3 continuation frames remain valid.
class ControlMessage {
 public:
  enum class Result { Ignore, More, Text, Audio, Invalid };
  void bind(char *buffer, size_t capacity) { buffer_ = buffer; capacity_ = capacity; reset(); }
  void reset() { used_ = 0; opcode_ = 0; }
  size_t size() const { return used_; }
  Result append(uint8_t opcode, const char *data, int length, int total, int offset) {
    if (opcode == 1 || opcode == 2) opcode_ = opcode;
    if (length < 0 || (length > 0 && !data)) return Result::Invalid;
    if (opcode_ == 2 && (opcode == 2 || opcode == 0)) return Result::Audio;
    if (opcode == 0) return Result::Invalid;
    if (opcode != 1) return Result::Ignore;
    if (offset == 0) used_ = 0;
    if (!buffer_ || total < 0 || offset < 0 || offset > total ||
        length > total - offset || size_t(total) > capacity_ ||
        size_t(offset) != used_ || size_t(length) > capacity_ - used_) return Result::Invalid;
    if (length) memcpy(buffer_ + used_, data, length);
    used_ += length;
    buffer_[used_] = 0; // caller supplies capacity + 1 bytes
    return used_ == size_t(total) ? Result::Text : Result::More;
  }
 private:
  char *buffer_ = nullptr;
  size_t capacity_ = 0;
  size_t used_ = 0;
  uint8_t opcode_ = 0;
};
