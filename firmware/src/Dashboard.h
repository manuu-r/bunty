#pragma once

#include <Arduino.h>

class Preferences;

// The Gauge dashboard: Bonjour discovery, the authenticated HTTP pull, the NVS
// snapshot that survives a reboot or an unreachable Mac, and the pages drawn
// from it.
//
// The snapshot is parsed once, when it is fetched or loaded, and the parsed
// model is what pages render from. Two bounded arenas alternate so a malformed
// update cannot destroy the currently displayed snapshot.
namespace dashboard {

// Loads any cached snapshot. `hardwareId` is the mDNS hostname Bunty claims.
void begin(Preferences &storage, const String &hardwareId);

// Resets fetch scheduling, paging, and failure counters. Called whenever the
// runtime (re)starts, on both the cold-boot and freshly-paired paths.
void restart();

bool available();
bool online();
void reportMemory();

// Queues a fetch if due or accepts a completed worker result. Never waits for
// the network. Returns true if the caller should redraw.
bool serviceFetch();

// True once the current page's dwell has elapsed. The caller decides whether
// the moment is right to advance (Bunty glances aside first).
bool pageDue();
void nextPage();

// Paints the current page, or a "syncing" placeholder when no valid snapshot
// is held, and restarts the page dwell.
void render();

// Drops cached mDNS answers. Bonjour can retain a stale address across a Mac
// sleep/wake or a DHCP lease change.
void forgetDiscovery();

// Makes the next serviceFetch() run immediately, without disturbing which page
// the carousel is showing.
void refreshSoon();

// Best-effort DELETE so Gauge can revoke this device's token before Bunty
// erases its own copy.
void notifyGaugeBeforeUnpair();

// Clears the cached snapshot and discovery state. The caller removes the NVS
// keys; this only drops what lives in RAM.
void clear();

}  // namespace dashboard
