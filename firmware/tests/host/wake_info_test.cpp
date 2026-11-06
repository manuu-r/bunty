#include "WakeInfo.h"
#include <cassert>
#include <string>
#include <cstdio>

int main() {
  using namespace wakeInfo;
  assert(last().sequence == 0);
  assert(std::string(direction(last(), 8)) == "unknown");  // nothing heard yet
  Record heard;
  heard.atMs = 1234; heard.bearingValid = true; heard.bearingRight = -31.5f; heard.confidence = 0.8f;
  heard.turned = true; heard.lookYaw = 31.5f; heard.conversationStarted = true;
  record(heard);
  Record got = last();
  assert(got.sequence == 1 && got.atMs == 1234 && got.turned && got.conversationStarted);
  assert(std::string(direction(got, 8)) == "left");  // negative bearing is Bunty's left
  heard.bearingRight = 40; record(heard);
  assert(last().sequence == 2 && std::string(direction(last(), 8)) == "right");
  heard.bearingRight = 5; record(heard);
  assert(std::string(direction(last(), 8)) == "ahead");  // inside the dead-band
  heard.bearingValid = false; heard.bearingRight = 50; record(heard);
  assert(std::string(direction(last(), 8)) == "unknown"); // an untrusted bearing is never reported as a side
  std::puts("Wake record: sequence, direction words and untrusted bearings passed");
}
