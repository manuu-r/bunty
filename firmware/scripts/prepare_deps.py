#!/usr/bin/env python3
"""Fetch checksum-pinned native C++ dependencies into an ignored build cache."""
import hashlib
import io
from pathlib import Path
import shutil
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / '.deps'
PACKAGES = (
    ('gfx', 'https://codeload.github.com/adafruit/Adafruit-GFX-Library/tar.gz/refs/tags/1.12.6',
     '3874218e5b9947ea11132cea5d6e8945f48306a5dada5b5b52f8f9ded53ee88f'),
    ('arduinojson', 'https://codeload.github.com/bblanchon/ArduinoJson/tar.gz/refs/tags/v7.4.3',
     'a2a8a277fc3cd0404499199bf1468682ea3688f354be0eaf6af8648366911257'),
)


def prepare(name, url, checksum):
    destination = CACHE / name
    stamp = checksum + ':native-v1'
    marker = destination / '.ready'
    if marker.is_file() and marker.read_text() == stamp:
        return
    print(f'Fetching {name} native dependency...', flush=True)
    with urllib.request.urlopen(url, timeout=60) as response:
        archive = response.read()
    if hashlib.sha256(archive).hexdigest() != checksum:
        raise RuntimeError(f'{name}: archive checksum mismatch')
    CACHE.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(dir=CACHE) as temporary:
        staging = Path(temporary)
        with tarfile.open(fileobj=io.BytesIO(archive), mode='r:gz') as tar:
            for member in tar.getmembers():
                if not member.isfile():
                    continue
                relative = Path(*Path(member.name).parts[1:])
                if relative.is_absolute() or '..' in relative.parts:
                    raise RuntimeError(f'{name}: unsafe archive path')
                if name == 'gfx':
                    if str(relative) not in ('Adafruit_GFX.cpp', 'Adafruit_GFX.h',
                                              'glcdfont.c', 'gfxfont.h', 'license.txt'):
                        continue
                    if relative.suffix == '.h':
                        relative = Path('include') / relative
                elif not (relative.parts and relative.parts[0] == 'src') and str(relative) != 'LICENSE.txt':
                    continue
                output = staging / relative
                output.parent.mkdir(parents=True, exist_ok=True)
                output.write_bytes(tar.extractfile(member).read())
        if name == 'gfx':
            header = staging / 'include' / 'Adafruit_GFX.h'
            source = header.read_text()
            old = ('#if ARDUINO >= 100\n#include "Arduino.h"\n#include "Print.h"\n'
                   '#else\n#include "WProgram.h"\n#endif\n#include "gfxfont.h"\n\n'
                   '#include <Adafruit_I2CDevice.h>\n#include <Adafruit_SPIDevice.h>')
            new = '#include "Runtime.h"\n#include "Print.h"\n#include "gfxfont.h"'
            signature = ('#if ARDUINO >= 100\n  virtual size_t write(uint8_t);\n'
                         '#else\n  virtual void write(uint8_t);\n#endif')
            if source.count(old) != 1 or source.count(signature) != 1:
                raise RuntimeError('gfx: native adaptation no longer matches upstream')
            header.write_text(source.replace(old, new).replace(signature, '  virtual size_t write(uint8_t);'))
        (staging / '.ready').write_text(stamp)
        if destination.exists():
            shutil.rmtree(destination)
        shutil.move(str(staging), destination)


if __name__ == '__main__':
    for package in PACKAGES:
        prepare(*package)
