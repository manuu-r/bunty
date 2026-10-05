#pragma once

#include "Runtime.h"

class Preferences;

// Gauge Accessory Protocol commissioning over Bluetooth LE Secure Connections.
//
// An on-screen Yes/No button answers the numeric-comparison prompt, Gauge writes a sequenced
// commissioning document over the MITM-protected link, and Bunty joins the
// Wi-Fi network it names. Everything the runtime needs afterwards is in NVS, so
// Bluetooth is released before audio claims DMA memory.
//
// The link is encrypted, authenticated, and bonded for this commissioning
// session. Gauge forgets that pairing when it disconnects. Any link key still
// stored here is cleared before advertising, so the next session compares
// numbers again. The static random address is minted once and kept for the
// life of the device, so a Mac always recognises this accessory as the same one.
namespace pairing {

void begin(Preferences &storage, const String &deviceName,
           const String &hardwareId);

// Begins advertising. Returns false and sets error() if the controller or its
// static random address was rejected.
bool start();
const char *error();
bool active();

// Drives the commissioning state machine. Returns true exactly once, when the
// device is commissioned and Bluetooth has been released: the caller should
// then greet the user and start the runtime.
bool service();

void stop();
// Reclaim controller/host static memory after commissioning; re-pairing
// subsequently requires the same reboot used by the existing Settings flow.
void releaseMemory();

}  // namespace pairing
