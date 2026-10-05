# Firmware libraries

Bunty's application code lives in `../src/`. ESP-IDF loads these supporting
libraries through `EXTRA_COMPONENT_DIRS` in the project CMake file.

- `bunty_platform/`: native timing, console, GPIO, PWM, CPU policy, NVS and Wi-Fi
  adapters. This is Bunty-owned source and is kept in version control.
- `gfx/`: build definition for Adafruit GFX 1.12.6. The dependency preparation
  script removes its Arduino/BusIO header dependencies and uses our minimal
  `Print` interface. Its BSD licence is cached at `../.deps/gfx/license.txt`.
- `arduinojson/`: build definition for ArduinoJson 7.4.3. It works without an
  Arduino core. Its MIT licence is cached at `../.deps/arduinojson/LICENSE.txt`.

`scripts/prepare_deps.py` downloads exact, SHA-256 checked release archives into
ignored `.deps/` on the first build or host check. Later builds use the cache.
Registry-managed mDNS and WebSocket dependencies live in ignored
`managed_components/`; versions are pinned by `src/idf_component.yml` and
`dependencies.lock`. No Arduino core or Muse SDK is linked.
