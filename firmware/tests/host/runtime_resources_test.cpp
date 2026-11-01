#include "RuntimeTiming.h"
#include "CaptureGate.h"
#include "TtsRequest.h"
#include <ArduinoJson.h>
#include <cassert>
#include <string>
#include <thread>
#include <cstdio>

int main() {
  runtime::Fade fade;
  fade.target(255,UINT32_MAX-100);
  assert(fade.sample(UINT32_MAX-100)==0);
  assert(fade.sample(11)==127); // 112 ms, through clock wrap
  fade.target(255,11); // repeated requests must not restart the transition
  assert(fade.sample(123)==255);
  fade.target(0,123);
  assert(fade.sample(193)==128);
  fade.target(255,193); // reverse smoothly from the current value
  assert(fade.sample(193)==128 && fade.sample(417)==255);

  audio::CaptureGate gate;
  gate.enable(true);
  const auto old=gate.session();
  gate.enable(false);
  gate.enable(true);
  assert(!gate.beginWrite(old));
  assert(gate.beginWrite(gate.session()));
  assert(gate.writing());
  gate.endWrite();
  std::atomic<bool> finished{false};
  std::atomic<unsigned> written{0};
  std::thread producer([&] {
    while(!finished) {
      const auto session=gate.session();
      if(gate.beginWrite(session)) { ++written; gate.endWrite(); }
    }
  });
  for(int i=0;i<10000;++i) {
    gate.enable(false);
    while(gate.writing()) std::this_thread::yield();
    const auto stopped=written.load();
    std::this_thread::yield();
    assert(written==stopped); // discard cannot race an old write
    gate.enable(true);
  }
  finished=true; producer.join();

  int32_t dc=0;
  for(int i=0;i<100000;++i) {
    const auto value=audio::removeDc((i&1)?262143:-262144,dc);
    assert(value>=-524288 && value<=524288);
  }
  char body[audio::kTtsBodyCapacity];
  const std::string values[]={"", "hello \"Muse\"\\\n\t", "हेलो",std::string(1023,'\1')};
  for(const auto &value:values) {
    const auto size=audio::ttsRequest(value.c_str(),body,sizeof(body));
    assert(size && size==strlen(body));
    JsonDocument document;
    assert(!deserializeJson(document,body));
    assert(document["text"].as<std::string>()==value);
  }
  char small[4]={'x','x','x','!'};
  assert(!audio::ttsRequest("overflow",small,3) && small[3]=='!');
  puts("Runtime fade timing, capture handoff, audio arithmetic and bounded TTS JSON passed");
}
