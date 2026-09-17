#pragma once

#include <Arduino.h>

class Preferences;
class TapInput;

// Gauge Accessory Protocol commissioning over Bluetooth LE Secure Connections.
//
// A tap answers the numeric-comparison prompt, Gauge writes a sequenced
// commissioning document over the MITM-protected link, and Bunty joins the
// Wi-Fi network it names. Everything the runtime needs afterwards is in NVS, so
// Bluetooth is released before audio claims DMA memory.
//
// The link is encrypted and authenticated but never bonded: pairing is one act
// with one purpose, and it leaves nothing behind on either side. The static
// random address is minted once and kept for the life of the device, so a Mac
// always recognises this accessory as the same one.
namespace pairing {

void begin(Preferences &storage, TapInput &taps, const String &deviceName,
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

}  // namespace pairing
