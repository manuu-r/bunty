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
    ('animatedgif', 'https://codeload.github.com/bitbank2/AnimatedGIF/tar.gz/c2478eca7aa2f3b7a09bdc583e5a4a43d03ac0d9',
     '272378b8ea8c9ac88d9cc587402e5b7aa6333669a6f0a88c6c9503f0af747fbc'),
    ('muse', 'https://codeload.github.com/facebookincubator/muse-gadget-sdk/tar.gz/74a5e2d7fc895f109f83a9a1dbed705dbcd8b1ff',
     '2d6d63ba7a5507b67322fedfc749ac7e440a13d30fccb7471cacbb0d017d25c3'),
    ('gfx', 'https://codeload.github.com/adafruit/Adafruit-GFX-Library/tar.gz/refs/tags/1.12.6',
     '3874218e5b9947ea11132cea5d6e8945f48306a5dada5b5b52f8f9ded53ee88f'),
    ('arduinojson', 'https://codeload.github.com/bblanchon/ArduinoJson/tar.gz/refs/tags/v7.4.3',
     'a2a8a277fc3cd0404499199bf1468682ea3688f354be0eaf6af8648366911257'),
)


def adapt_gif(directory):
    # Upstream asks for another entire palette even when its initial read
    # already contains it; small valid GIFs then fail at EOF. Read only missing
    # palette bytes. Pin and test this adaptation like the native GFX changes.
    file = directory / 'src/gif.inl'
    old = """            i = (*pPage->pfnRead)(&pPage->GIFFile, &pPage->ucFileBuf[iBytesRead], 3*(1<<iColorTableBits));
            iBytesRead += i;
            if (iColorTableBits != 1 && i < 3*(1<<iColorTableBits)) { // file too small
                pPage->iError = GIF_BAD_FILE;
                return 0;
            }"""
    new = """            int missing = iOffset + 3*(1<<iColorTableBits) - iBytesRead;
            if (missing > 0) {
                i = (*pPage->pfnRead)(&pPage->GIFFile, &pPage->ucFileBuf[iBytesRead], missing);
                iBytesRead += i;
                if (i != missing) {
                    pPage->iError = GIF_BAD_FILE;
                    return 0;
                }
            }"""
    source = file.read_text()
    # Migrate the earlier palette-only adaptation as well as the pinned source.
    if new not in source and "int wanted = iOffset + 3*(1<<iColorTableBits) + 32;" not in source:
        if source.count(old) != 1:
            raise RuntimeError('animatedgif: palette adaptation no longer matches')
        source = source.replace(old, new)
    before = "int missing = iOffset + 3*(1<<iColorTableBits) - iBytesRead;"
    after = "int wanted = iOffset + 3*(1<<iColorTableBits) + 32;\n            if (wanted > pPage->GIFFile.iSize - iStartPos) wanted = pPage->GIFFile.iSize - iStartPos;\n            int missing = wanted - iBytesRead;"
    if after not in source:
        if source.count(before) != 1: raise RuntimeError('animatedgif: global lookahead patch mismatch')
        source = source.replace(before, after)
    before_local = "        iBytesRead += (*pPage->pfnRead)(&pPage->GIFFile, &pPage->ucFileBuf[iBytesRead], j*3);"
    after_local = """        int wanted = iOffset + j*3 + 32;
        if (wanted > pPage->GIFFile.iSize - iStartPos) wanted = pPage->GIFFile.iSize - iStartPos;
        int missing = wanted - iBytesRead;
        if (missing > 0) {
            int got = (*pPage->pfnRead)(&pPage->GIFFile, &pPage->ucFileBuf[iBytesRead], missing);
            if (got != missing) { pPage->iError = GIF_BAD_FILE; return 0; }
            iBytesRead += got;
        }"""
    if after_local not in source:
        if source.count(before_local) != 1: raise RuntimeError('animatedgif: local lookahead patch mismatch')
        source = source.replace(before_local, after_local)
    file.write_text(source)


def prepare(name, url, checksum):
    destination = CACHE / name
    stamp = checksum + ':native-v1'
    marker = destination / '.ready'
    if marker.is_file() and marker.read_text() == stamp:
        if name == 'animatedgif': adapt_gif(destination)
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
                elif name == 'muse':
                    if not (relative.parts and relative.parts[0] == 'esp32') and str(relative) != 'LICENSE':
                        continue
                elif not (relative.parts and relative.parts[0] == 'src') and str(relative) not in ('LICENSE.txt','LICENSE'):
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
        if name == 'animatedgif': adapt_gif(staging)
        (staging / '.ready').write_text(stamp)
        if destination.exists():
            shutil.rmtree(destination)
        shutil.move(str(staging), destination)


if __name__ == '__main__':
    for package in PACKAGES:
        prepare(*package)
