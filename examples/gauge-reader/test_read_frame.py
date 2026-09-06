# origin: PUBLIC — generated arrays only; no footage, screenshots or reading fixtures.
import os
from pathlib import Path
import unittest

import numpy as np
from PIL import Image

from read_frame import GaugeReader, crop_bytes, decode_with_count, inspect_layout


class ExampleTests(unittest.TestCase):
    def test_negatives(self):
        for shade in (0, 32, 127, 255):
            self.assertEqual(inspect_layout(np.full((1080, 1920, 3), shade, np.uint8)), "rejected")
        rng = np.random.default_rng(7)
        for _ in range(4):
            pixels = rng.integers(0, 256, (1080, 1920, 3), dtype=np.uint8)
            self.assertEqual(inspect_layout(pixels), "rejected")
        self.assertEqual(inspect_layout(np.zeros((720, 1280, 3), np.uint8)), "unsupported_geometry")
        with self.assertRaises(ValueError):
            inspect_layout(np.zeros((1080, 1920, 3), np.float32))

    def test_crop_shape_and_bar_orientation(self):
        image = Image.new("RGB", (1920, 1080))
        image.paste((0, 0, 255), (956, 628, 999, 761))
        image.paste((255, 0, 0), (956, 761, 999, 894))
        crops = crop_bytes(image)
        self.assertEqual(crops.shape, (4, 64, 192, 3))
        self.assertEqual(crops.dtype, np.uint8)
        # Bottom becomes left after clockwise rotation; no image reflection.
        np.testing.assert_array_equal(crops[2, 32, 0], [255, 0, 0])
        np.testing.assert_array_equal(crops[2, 32, -1], [0, 0, 255])
        self.assertTrue((crops[0, :, :61] == 0).all())
        with self.assertRaises(ValueError):
            crop_bytes(image.resize((1280, 720)))

    def test_count_constrained_repeated_digits(self):
        logits = np.full((11, 24), -12, dtype=np.float32)
        logits[2] = 6  # token 2 means digit 1, not digit 2
        logits[0, 5::6] = 10  # blanks permit repeated copies of the same digit
        for length in (1, 2, 3):
            counts = np.full(3, -4, dtype=np.float32)
            counts[length - 1] = 4
            self.assertEqual(decode_with_count(logits, counts), "1" * length)
        with self.assertRaises(ValueError):
            decode_with_count(logits * np.nan, np.zeros(3, np.float32))

    def test_model_checksum_failure(self):
        import tempfile
        with tempfile.TemporaryDirectory() as directory:
            model = Path(directory) / "not-a-model.onnx"
            model.write_bytes(b"public synthetic invalid model")
            with self.assertRaises(ValueError):
                GaugeReader(model)

    @unittest.skipUnless(os.environ.get("OMATRACK_EXAMPLE_MODEL"), "set OMATRACK_EXAMPLE_MODEL for downloaded-model smoke test")
    def test_downloaded_model_rejects_blank(self):
        reader = GaugeReader(os.environ["OMATRACK_EXAMPLE_MODEL"])
        result = reader.read(Image.new("RGB", (1920, 1080)))
        self.assertEqual(result["status"], "rejected")
        self.assertTrue(result["visited"])
        self.assertFalse(result["layout_supported"])
        self.assertTrue(all(value is None for value in result["observations"].values()))
        self.assertFalse(any(result["known"].values()))


if __name__ == "__main__":
    unittest.main()
