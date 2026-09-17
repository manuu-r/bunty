#include <array>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
#include "LocalVad.h"

using namespace cfg::voice;
static size_t vadAllocations = 0, vadFrees = 0, vadBytes = 0;
static bool failVadAllocation = false;
extern "C" void *vad_test_malloc(size_t size) {
  ++vadAllocations;
  vadBytes = size;
  return failVadAllocation ? nullptr : std::malloc(size);
}
extern "C" void vad_test_free(void *p) { ++vadFrees; std::free(p); }

static void windowTests() {
  ConversationWindow window;
  assert(!window.armed() && !window.expire(4000));
  window.arm(100);
  assert(window.armed());
  assert(!window.expire(4099));
  assert(window.expire(4100));
  assert(!window.armed() && !window.expire(4101));
  window.arm(100);
  window.arm(4000);  // New tap/reply starts a fresh four-second window.
  assert(!window.expire(4100));
  assert(window.expire(8000));
  window.arm(UINT32_MAX - 2000);
  assert(!window.expire(1998));
  assert(window.expire(1999));
  window.arm(uint32_t(0) - kConversationWindowMs); // Reserved zero deadline.
  assert(window.armed() && !window.expire(0) && window.expire(1));
  window.arm(100);
  window.close();
  assert(!window.armed());
}

static void gateTests() {
  SpeechGate gate;
  for (int i = 0; i < 1000; ++i) gate.observe(false);
  assert(!gate.started() && !gate.timedOut());
  // An isolated impact, even with WebRTC's short hangover, is insufficient.
  for (unsigned i = 0; i < 4; ++i) gate.observe(true);
  gate.observe(false);
  assert(!gate.started());
  for (unsigned i = 0; i < kSpeechStartMs / kFrameDurationMs - 1; ++i) {
    gate.observe(true);
    assert(!gate.started());
  }
  gate.observe(true);
  assert(gate.started() && !gate.timedOut());
  // Confirmed onset survives connection establishment and pre-roll flushing.
  for (unsigned i = 0; i < kNoSpeechTimeoutMs / kFrameDurationMs - 1; ++i) {
    gate.observe(false);
    assert(!gate.timedOut());
  }
  gate.observe(false);
  assert(gate.timedOut());
  // Reconfirmed speech, not a single loud frame, refreshes the watchdog.
  for (unsigned i = 0; i < kSpeechStartMs / kFrameDurationMs; ++i) gate.observe(true);
  assert(!gate.timedOut());
  for (unsigned i = 0; i < kNoSpeechTimeoutMs / kFrameDurationMs - 1; ++i) {
    gate.observe(false);
    assert(!gate.timedOut());
  }
  gate.observe(true); // Isolated junk cannot keep a socket alive.
  assert(gate.timedOut());
  for (int i = 0; i < 100000; ++i) gate.observe(false); // Counter saturates.
  assert(gate.timedOut());
  gate.reset();
  assert(!gate.started() && !gate.timedOut());
}

static void watchdogTests() {
  using Reason = InputSpeechWatchdog::Reason;
  InputSpeechWatchdog watchdog;
  watchdog.begin(100);
  assert(watchdog.reason(4099) == Reason::None);
  assert(watchdog.reason(4100) == Reason::LocalNonSpeech);
  watchdog.begin(100);
  // Lots of noise/transport activity is not speech; empty transcripts don't help.
  for (uint32_t now = 100; now <= 4100; now += 20) {
    watchdog.observeLocal(now, false);
    watchdog.observeTranscript(now, "   \t");
  }
  assert(watchdog.reason(4100) == Reason::LocalNonSpeech);
  watchdog.begin(100);
  watchdog.observeLocal(4100, true); // False positive from local detector.
  assert(watchdog.reason(4100) == Reason::NoTranscriptProgress);
  watchdog.observeTranscript(4100, "hello");
  assert(watchdog.reason(4100) == Reason::None);
  watchdog.observeLocal(8100, true);
  watchdog.observeTranscript(8100, "hello"); // Repeated update != progress.
  assert(watchdog.reason(8100) == Reason::NoTranscriptProgress);
  watchdog.observeTranscript(8100, "hello Bunty");
  assert(watchdog.reason(8100) == Reason::None);
  watchdog.observeLocal(8101, true);
  watchdog.observeTranscript(8101, "hello Bunty!");
  assert(watchdog.reason(8100) == Reason::None); // Concurrent timestamp publication.
  watchdog.begin(UINT32_MAX - 2000);
  assert(watchdog.reason(1998) == Reason::None);
  assert(watchdog.reason(1999) == Reason::LocalNonSpeech);
  for (const char *event : {"input.ended", "input.pause", "assistant.thinking"}) {
    assert(gatewayEndsInput(event));
  }
  for (const char *event : {"speech.maybe_finished", "turn_detection", "speech.started", "speech.resumed", "pong", "transcript.partial"}) {
    assert(!gatewayEndsInput(event));
  }
}

static uint32_t little(const uint8_t *data, size_t bytes) {
  uint32_t value = 0;
  for (size_t i = 0; i < bytes; ++i) value |= uint32_t(data[i]) << (8 * i);
  return value;
}

static std::vector<int16_t> readFixture(const char *path) {
  std::ifstream file(path, std::ios::binary);
  assert(file.good());
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
  assert(bytes.size() >= 12 && !memcmp(bytes.data(), "RIFF", 4));
  assert(!memcmp(bytes.data() + 8, "WAVE", 4));
  bool formatSeen = false;
  for (size_t offset = 12; offset + 8 <= bytes.size();) {
    const uint32_t size = little(bytes.data() + offset + 4, 4);
    assert(size <= bytes.size() - offset - 8);
    const auto *data = bytes.data() + offset + 8;
    if (!memcmp(bytes.data() + offset, "fmt ", 4)) {
      assert(size >= 16 && little(data, 2) == 1 && little(data + 2, 2) == 1);
      assert(little(data + 4, 4) == kVadSampleRate && little(data + 14, 2) == 16);
      formatSeen = true;
    } else if (!memcmp(bytes.data() + offset, "data", 4)) {
      assert(formatSeen && size % 2 == 0);
      std::vector<int16_t> samples(size / 2);
      for (size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<int16_t>(little(data + i * 2, 2));
      }
      return samples;
    }
    offset += 8 + size + (size % 2);
  }
  assert(false && "missing PCM data");
  return {};
}

static void detectorTests(const char *fixture) {
  LocalVad vad;
  std::array<int16_t, kFrameSamples> zeros{};
  assert(vad.process(zeros.data(), zeros.size()) == -1);
  failVadAllocation = true;
  assert(!vad.begin());
  vad.end();
  failVadAllocation = false;
  assert(vad.begin());
  const size_t calls = vadAllocations;
  assert(vad.begin() && vadAllocations == calls);
  assert(vad.process(nullptr, kFrameSamples) == -1);
  assert(vad.process(zeros.data(), kFrameSamples - 1) == -1);
  for (int i = 0; i < 1000; ++i) {
    assert(vad.reset());
    assert(vad.process(zeros.data(), zeros.size()) == 0);
  }
  auto samples = readFixture(fixture);
  const auto original = samples;
  assert(vad.reset()); // Reference starts with a fresh engine, no warm-up PCM.
  Fvad *reference = fvad_new();
  assert(reference && fvad_set_mode(reference, 2) == 0);
  assert(fvad_set_sample_rate(reference, 16000) == 0);
  const size_t processingCalls = vadAllocations;
  unsigned speechFrames = 0, totalFrames = 0;
  SpeechGate gate;
  for (size_t offset = 0; offset + kFrameSamples <= samples.size(); offset += kFrameSamples) {
    const int speech = vad.process(samples.data() + offset, kFrameSamples);
    assert(speech >= 0);
    assert(speech == fvad_process(reference, samples.data() + offset, kFrameSamples));
    speechFrames += speech;
    ++totalFrames;
    uint64_t sum = 0;
    for (size_t i = 0; i < kFrameSamples; ++i) {
      const int64_t sample = samples[offset + i];
      sum += sample * sample;
    }
    gate.observe(speech == 1 && std::sqrt(double(sum) / kFrameSamples) >= kMinimumSpeechRms);
  }
  assert(samples == original); // Never alter cloud PCM.
  assert(gate.started());
  // Compare every decision to a fresh, explicitly configured engine above.
  // The checked-in upstream wavtest.expect differs by one frame on this host;
  // do not confuse that legacy golden count with an accuracy measurement.
  assert(totalFrames == 270 && speechFrames > 200 && speechFrames < totalFrames);
  for (unsigned i = 0; i < 225; ++i) {
    gate.observe(vad.process(zeros.data(), zeros.size()) == 1);
  }
  assert(gate.timedOut());
  assert(vadAllocations == processingCalls && vadFrees == 0);
  std::cout << "WebRTC fixture: " << speechFrames << '/' << totalFrames
            << " speech frames; instance allocation=" << vadBytes << " bytes on host\n";
  vad.end();
  vad.end();
  fvad_free(reference);
  assert(vadFrees == 2);
}

int main(int argc, char **argv) {
  assert(argc == 2);
  windowTests();
  gateTests();
  watchdogTests();
  detectorTests(argv[1]);
  std::cout << "VAD, gateway endpoints, noise/transcript watchdogs, 4000 ms timeout, wraparound, and allocation tests passed\n";
}
