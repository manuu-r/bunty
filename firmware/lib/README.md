# Firmware libraries

Bunty's application code lives in `../src/`. ESP-IDF loads these supporting
libraries through `EXTRA_COMPONENT_DIRS` in the project CMake file.

- `bunty_platform/`: native timing, console, GPIO, PWM, CPU policy, NVS and Wi-Fi
  adapters. This is Bunty-owned source and is kept in version control.
- `gfx/`: build definition for Adafruit GFX 1.12.6. The dependency preparation
  script removes its Arduino/BusIO header dependencies and uses our minimal
  `Print` interface. Its BSD licence is cached at `../.deps/gfx/license.txt`.
- `animatedgif/`: build definition for the Apache 2.0 AnimatedGIF decoder, pinned
  to commit `c2478eca7aa2f3b7a09bdc583e5a4a43d03ac0d9` with SHA-256 validation.
  Its source/licence are cached under `../.deps/animatedgif/`; dependency
  preparation fixes a short-file palette over-read in the pinned parser.
- `arduinojson/`: build definition for ArduinoJson 7.4.3. It works without an
  Arduino core. Its MIT licence is cached at `../.deps/arduinojson/LICENSE.txt`.

`scripts/prepare_deps.py` downloads exact, SHA-256 checked release archives into
ignored `.deps/` on the first build or host check. Later builds use the cache.
Registry-managed mDNS and WebSocket dependencies live in ignored
`managed_components/`; versions are pinned by `src/idf_component.yml` and
`dependencies.lock`. No Arduino core is linked. The pinned Muse SDK supplies the connection and chat
protocol; `muse_core/` contains its build definitions and Bunty hook declarations.
The SDK source and Apache 2.0 licence are cached under `../.deps/muse/`.
