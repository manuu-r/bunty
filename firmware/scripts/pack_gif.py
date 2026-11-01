#!/usr/bin/env python3
"""Create exact Muse tool calls for direct GIF transfer; does not send or host files."""
import argparse
import base64
import json
import pathlib
import struct
import uuid
import zlib

MAX_BYTES = 1048576
CHUNK_BYTES = 6144


def calls(data, transfer_id=None, row=-1, gesture=None, speed="fast"):
    if not 14 <= len(data) <= MAX_BYTES or data[:6] not in (b"GIF87a", b"GIF89a"):
        raise ValueError("Expected a GIF file of 14 to 1048576 bytes")
    width, height = struct.unpack_from("<HH", data, 6)
    if not 1 <= width <= 240 or not 1 <= height <= 320:
        raise ValueError("Resize the GIF canvas to at most 240x320 first")
    if not -1 <= row <= 319 or row >= 0 and row + height > 320:
        raise ValueError("GIF placement exceeds the screen")
    if gesture not in (None, "neutral", "nod", "shake", "curious", "happy") or speed not in ("slow", "normal", "fast"):
        raise ValueError("Invalid gesture or speed")
    if len(data) <= CHUNK_BYTES:
        params = {"data_base64": base64.b64encode(data).decode("ascii")}
        if gesture is not None:
            params.update(gesture=gesture, speed=speed)
        if row != -1:
            params["row"] = row
        yield {"command": "display.show_gif", "params": params}
        return
    transfer_id = transfer_id or uuid.uuid4().hex
    if not 1 <= len(transfer_id) <= 32 or any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-" for c in transfer_id):
        raise ValueError("Invalid transfer ID")
    def call(op, **params):
        return {"command": "bunty.gif_transfer", "params": {"op": op, "id": transfer_id, **params}}
    if gesture is not None:
        yield {"command": "bunty.gesture", "params": {"gesture": gesture, "speed": speed, "when": "display"}}
    yield call("begin", bytes=len(data), crc32=f"{zlib.crc32(data):08x}", row=row)
    for offset in range(0, len(data), CHUNK_BYTES):
        yield call("chunk", offset=offset, data_base64=base64.b64encode(data[offset:offset + CHUNK_BYTES]).decode("ascii"))
    yield call("commit")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gif", type=pathlib.Path)
    parser.add_argument("--row", type=int, default=-1)
    parser.add_argument("--id")
    parser.add_argument("--gesture", choices=["neutral", "nod", "shake", "curious", "happy"])
    parser.add_argument("--speed", choices=["slow", "normal", "fast"], default="fast")
    args = parser.parse_args()
    try:
        with args.gif.open("rb") as stream:
            data = stream.read(MAX_BYTES + 1)
        # Validate before emitting any command.
        output = list(calls(data, args.id, args.row, args.gesture, args.speed))
    except (OSError, ValueError) as error:
        parser.error(str(error))
    for item in output:
        print(json.dumps(item, separators=(",", ":")))


if __name__ == "__main__":
    main()
