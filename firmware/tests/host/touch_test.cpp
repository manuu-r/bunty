#include "TouchPress.h"
#include <cassert>
#include <cstdio>
using K = touch::Event::Kind;
int main() {
  touch::PressTracker p;
  auto sample = [&](int contact, int x, int y, unsigned now) {
    return p.sample(contact,x,y,now,650,120,24).kind;
  };
  assert(sample(1,30,180,200)==K::kNone);
  assert(sample(0,0,0,240)==K::kTap);
  assert(sample(0,0,0,260)==K::kNone);
  assert(sample(1,30,180,270)==K::kNone);
  assert(sample(0,0,0,290)==K::kNone);
  assert(sample(1,30,180,500)==K::kNone);
  assert(sample(1,30,180,1150)==K::kLongPress);
  assert(sample(1,30,180,1200)==K::kNone);
  assert(sample(0,0,0,1250)==K::kNone);
  // Dragging out and back never activates the starting button.
  sample(1,30,180,1500);sample(1,90,180,1530);sample(1,30,180,1560);
  assert(sample(0,0,0,1600)==K::kNone);
  // I2C loss cannot confirm pairing, even if contact resumes before release.
  sample(1,30,180,1800);sample(-1,0,0,1820);sample(1,30,180,2100);
  assert(sample(0,0,0,2200)==K::kNone);
  sample(1,30,180,2400);assert(sample(0,0,0,2450)==K::kTap);
  puts("Touch press/release, hold, debounce, drag and I2C recovery passed");
}
