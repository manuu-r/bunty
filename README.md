# Bunty

Bunty is an ESP32-S3 desk companion and the reference client for the
[Gauge Accessory Protocol](https://github.com/manuu-r/homebrew-tap/blob/main/docs/accessory-protocol.md).

| Folder | Contents |
| --- | --- |
| [`firmware/`](firmware) | PlatformIO firmware: pairing, dashboard, display, taps, servos, voice |
| [`iot-gateway/`](iot-gateway) | Cloudflare Worker that brokers Bunty's voice turns and device credentials |

Keep both folders side by side: the firmware's provisioned upload
(`BUNTY_PROVISION_IOT=1`) runs `iot-gateway/scripts/devices.mjs` from here.

```sh
cd firmware
pio run -e bunty -t upload            # original carrier board
pio run -e bunty-waveshare -t upload  # Waveshare ESP32-S3-Touch-LCD-2
```
