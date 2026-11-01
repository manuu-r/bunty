import base64
import importlib.util
from pathlib import Path
import unittest
import zlib

spec = importlib.util.spec_from_file_location("pack_gif", Path(__file__).resolve().parents[2] / "scripts/pack_gif.py")
pack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pack)


class PackGifTests(unittest.TestCase):
    def test_byte_exact_calls_and_checksum(self):
        data = b"GIF89a\x01\x00\x01\x00\x00\x00\x00" + bytes(range(256)) * 30
        calls = list(pack.calls(data, "test-1"))
        self.assertEqual(calls[0]["params"]["crc32"], f"{zlib.crc32(data):08x}")
        self.assertEqual(calls[0]["params"]["bytes"], len(data))
        reconstructed = bytearray()
        for call in calls[1:-1]:
            params = call["params"]
            self.assertEqual(params["offset"], len(reconstructed))
            self.assertLessEqual(len(params["data_base64"]), 8192)
            reconstructed.extend(base64.b64decode(params["data_base64"], validate=True))
        self.assertEqual(reconstructed, data)
        self.assertEqual(calls[-1]["params"]["op"], "commit")
        self.assertTrue(all(c["command"] == "bunty.gif_transfer" and c["params"]["id"] == "test-1" for c in calls))

    def test_inline_boundary(self):
        header = b"GIF89a\x01\x00\x01\x00\x00\x00\x00"
        for n in [14, 6144, 6145]:
            data = header + b"x" * (n - len(header))
            output = list(pack.calls(data, "inline"))
            if n <= 6144:
                self.assertEqual(len(output), 1)
                self.assertEqual(output[0]["command"], "display.show_gif")
                self.assertEqual(set(output[0]["params"]), {"data_base64"})
                self.assertEqual(base64.b64decode(output[0]["params"]["data_base64"]), data)
            else:
                self.assertEqual(len(output), 4)
                self.assertEqual(output[0]["params"]["op"], "begin")

    def test_combined_gif_and_movement(self):
        data = b"GIF89a\x01\x00\x01\x00\x00\x00\x00\x3b"
        output = list(pack.calls(data, gesture="happy", speed="fast"))
        self.assertEqual(len(output), 1)
        self.assertEqual(output[0]["params"]["gesture"], "happy")
        self.assertEqual(output[0]["params"]["speed"], "fast")
        self.assertEqual(base64.b64decode(output[0]["params"]["data_base64"]), data)
        with self.assertRaises(ValueError):
            list(pack.calls(data, gesture="giant"))
        with self.assertRaises(ValueError):
            list(pack.calls(data, speed="turbo"))

    def test_input_bounds(self):
        valid = b"GIF89a\x01\x00\x01\x00\x00\x00\x00\x3b"
        for data in [b"", b"zip" * 8, valid + b"x" * pack.MAX_BYTES, b"GIF89a\xf1\x00\x01\x00\x00\x00\x00\x3b"]:
            with self.assertRaises(ValueError):
                list(pack.calls(data))
        for row in [-2, 320]:
            with self.assertRaises(ValueError):
                list(pack.calls(valid, row=row))
        with self.assertRaises(ValueError):
            list(pack.calls(valid + b"x" * 6144, transfer_id="bad/id"))
        self.assertEqual(len(list(pack.calls(valid))), 1)
