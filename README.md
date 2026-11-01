# Bunty

Bunty is an ESP32-S3 desk companion powered by Muse, with voice conversations,
an animated face, touch controls, and head movement.

| Folder | Contents |
| --- | --- |
| [`firmware/`](firmware) | Native ESP-IDF 6.0.1 firmware: Muse pairing, display, touch, servos, voice |
| [`iot-gateway/`](iot-gateway) | Cloudflare Worker for device credentials, TTS, and the legacy voice pipeline |

Keep both folders side by side for explicit gateway provisioning.

```sh
cd firmware
./scripts/idf.sh build
./scripts/idf.sh -p /dev/cu.usbmodemXXXX flash monitor
```

See [`firmware/README.md`](firmware/README.md) for toolchain installation,
credential handling, and the hardware verification checklist. Muse handles pairing and chat, with local VAD for hands-free turns; the existing gateway supplies TTS.
See [`firmware/MUSE.md`](firmware/MUSE.md) for setup and verification status.
