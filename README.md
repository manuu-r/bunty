# Bunty

Bunty is an ESP32-S3 desk companion and the reference client for the
[Gauge Accessory Protocol](https://github.com/manuu-r/homebrew-tap/blob/main/docs/accessory-protocol.md).

| Folder | Contents |
| --- | --- |
| [`firmware/`](firmware) | Native ESP-IDF 6.0.1 firmware: pairing, dashboard, display, touch, servos, voice |
| [`iot-gateway/`](iot-gateway) | Cloudflare Worker that brokers Bunty's voice turns and device credentials |

Keep both folders side by side for explicit gateway provisioning.

```sh
cd firmware
./scripts/idf.sh build
./scripts/idf.sh -p /dev/cu.usbmodemXXXX flash monitor
```

See [`firmware/README.md`](firmware/README.md) for toolchain installation,
credential handling, and the hardware verification checklist. The migration
preserves the current Gauge and Cloudflare behaviour; Muse is a later integration.
