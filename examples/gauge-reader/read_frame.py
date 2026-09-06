#!/usr/bin/env -S uv run --script
# origin: PUBLIC — standalone example; no images, credentials or training data.
# Adapted from Omatrack's MIT-licensed GaugeReader runtime. See NOTICE and
# LICENSE-MIT-OMATRACK-CODE.txt in the published model repository.
"""Read four visible HUD fields from a LOCAL 1920x1080 RGB frame, on CPU.

There is no network code, upload, PyTorch dependency or research-checkout import.
This is a fixed-layout heuristic + crop reader, NOT an arbitrary HUD detector.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import time

import numpy as np
from PIL import Image
import onnxruntime as ort

MODEL_SHA256 = "97029f70068f4ec276b3d6bc28810763275806f579d91ddd4701b544af392147"
MODEL_SIZE = 2_213_746
FIELDS = ("gear", "stint_lap", "brake_fill_pct", "throttle_fill_pct")
CROP_BOXES = (
    (1399, 1010, 1475, 1079),
    (408, 994, 479, 1044),
    (956, 628, 999, 894),
    (1011, 628, 1055, 894),
)
SIZE = (192, 64)
METADATA = {
    "omatrack.contract": "omatrack-crop-count-v1",
    "omatrack.checkpoint_sha256": "2b1bedae45f08c9187e8e26a92cc1a01db30bda812e8fb7d85fe51368a80faeb",
    "omatrack.preprocessing": "pillow-rgb-bilinear-22bit-crop-count-v1",
    "omatrack.layout": "tds_aim_orange-1920x1080",
    "omatrack.decoder": "count-argmax-plus-one-ctc-prefix-beam-10-per-length",
    "omatrack.fields": ",".join(FIELDS),
}
NEG = -math.inf


def _components(pixels):
    return np.moveaxis(pixels.astype(np.float64), -1, 0)


def _red(pixels):
    r, g, b = _components(pixels)
    return (r >= 18) & (r > 1.6 * g + 6) & (r > 1.4 * b + 6)


def _green(pixels):
    r, g, b = _components(pixels)
    return (g >= 20) & (g > 1.5 * r + 8) & (g > 1.3 * b + 8)


def _orange(pixels):
    r, g, b = _components(pixels)
    return (r > 45) & (g > 18) & (r > 1.2 * g) & (b < 0.5 * g)


def _white(pixels):
    lo, hi = pixels.min(axis=-1), pixels.max(axis=-1)
    return (lo > 160) & (hi - lo < 55)


def _fraction(pixels, box, predicate):
    left, top, right, bottom = box
    return float(predicate(pixels[top:bottom:2, left:right:2]).mean())


def _edges(pixels, center, left, right, predicate):
    return float((predicate(pixels[646:880:4, center])
                  & ~predicate(pixels[646:880:4, left])
                  & ~predicate(pixels[646:880:4, right])).mean())


def inspect_layout(pixels: np.ndarray) -> str:
    """Return supported/rejected/unsupported_geometry; no model outputs used."""
    if pixels.dtype != np.uint8 or pixels.ndim != 3 or pixels.shape[-1] != 3:
        raise ValueError("expected RGB uint8 HWC pixels")
    if pixels.shape != (1080, 1920, 3):
        return "unsupported_geometry"
    ticks = _fraction(pixels, (1440, 956, 1880, 981), _white)
    supported = (
        _fraction(pixels, (968, 642, 984, 880), _red) >= 0.94
        and _fraction(pixels, (1024, 642, 1040, 880), _green) >= 0.94
        and _edges(pixels, 976, 960, 992, _red) >= 0.90
        and _edges(pixels, 1032, 1016, 1048, _green) >= 0.90
        and _fraction(pixels, (1415, 855, 1875, 877), _orange) >= 0.82
        and _fraction(pixels, (1410, 988, 1875, 1006), _orange) >= 0.82
        and 0.012 <= ticks <= 0.15
    )
    return "supported" if supported else "rejected"


def crop_bytes(image: Image.Image) -> np.ndarray:
    """Exact Pillow preprocessing; returns uint8 NHWC [4,64,192,3]."""
    if image.size != (1920, 1080) or image.mode != "RGB":
        raise ValueError("expected an unscaled full-resolution 1920x1080 RGB frame")
    crops = []
    for field, box in enumerate(CROP_BOXES):
        crop = image.crop(box)
        if field >= 2:
            crop = crop.transpose(Image.Transpose.ROTATE_270)
            crop = crop.resize(SIZE, Image.Resampling.BILINEAR)
        else:
            ratio = min(SIZE[0] / crop.width, SIZE[1] / crop.height)
            crop = crop.resize((max(1, round(crop.width * ratio)),
                                max(1, round(crop.height * ratio))), Image.Resampling.BILINEAR)
            padded = Image.new("RGB", SIZE, (0, 0, 0))
            padded.paste(crop, ((SIZE[0] - crop.width) // 2, (SIZE[1] - crop.height) // 2))
            crop = padded
        crops.append(np.asarray(crop, dtype=np.uint8))
    return np.stack(crops)


def _add(a: float, b: float) -> float:
    if a == NEG:
        return b
    if b == NEG:
        return a
    return max(a, b) + math.log1p(math.exp(-abs(a - b)))


def decode_with_count(logits: np.ndarray, counts: np.ndarray) -> str:
    """Count-constrained CTC: beam width TEN PER LENGTH, blank token=0."""
    if logits.shape != (11, 24) or counts.shape != (3,):
        raise ValueError("wrong digit/count tensor shape")
    if not np.isfinite(logits).all() or not np.isfinite(counts).all():
        raise ValueError("nonfinite digit/count tensor")
    length = int(counts.argmax()) + 1
    rows = logits.astype(np.float64).T
    rows -= rows.max(axis=1, keepdims=True)
    rows -= np.log(np.exp(rows).sum(axis=1, keepdims=True))
    beams = {(): (0.0, NEG)}
    for step in rows:
        next_beams = {}

        def update(prefix, blank=NEG, nonblank=NEG):
            a, b = next_beams.get(prefix, (NEG, NEG))
            next_beams[prefix] = (_add(a, blank), _add(b, nonblank))

        for prefix, (pb, pn) in beams.items():
            total = _add(pb, pn)
            update(prefix, blank=total + step[0])
            for token in range(1, 11):
                if prefix and prefix[-1] == token:
                    update(prefix, nonblank=pn + step[token])
                    if len(prefix) < length:
                        update(prefix + (token,), nonblank=pb + step[token])
                elif len(prefix) < length:
                    update(prefix + (token,), nonblank=total + step[token])
        # Python insertion order and stable ties match the reference decoder.
        beams = {}
        for n in range(length + 1):
            group = [(p, scores) for p, scores in next_beams.items() if len(p) == n]
            beams.update(sorted(group, key=lambda item: _add(*item[1]), reverse=True)[:10])
    complete = [prefix for prefix in beams if len(prefix) == length]
    best = max(complete, key=lambda p: _add(*beams[p])) if complete else ()
    return "".join(str(token - 1) for token in best)


def _empty_result():
    return {"status": "not_checked", "visited": False, "layout_supported": False,
            "observations": dict.fromkeys(FIELDS), "known": dict.fromkeys(FIELDS, False),
            "unknown_reason": dict.fromkeys(FIELDS, "not observed"),
            "latency_ms": 0.0, "error": None}


class GaugeReader:
    """Reusable CPU session. No image data leaves this process."""

    def __init__(self, model: str | Path):
        path = Path(model)
        if path.stat().st_size != MODEL_SIZE:
            raise ValueError("model size does not match this reviewed release")
        data = path.read_bytes()
        if hashlib.sha256(data).hexdigest() != MODEL_SHA256:
            raise ValueError("model SHA256 does not match this reviewed release")
        options = ort.SessionOptions()
        options.intra_op_num_threads = 1
        options.inter_op_num_threads = 1
        options.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
        self.session = ort.InferenceSession(data, sess_options=options, providers=["CPUExecutionProvider"])
        actual_metadata = self.session.get_modelmeta().custom_metadata_map
        if any(actual_metadata.get(key) != value for key, value in METADATA.items()):
            raise ValueError("model contract/provenance mismatch")
        inputs, outputs = self.session.get_inputs(), self.session.get_outputs()
        wanted = [("crops", [4, 3, 64, 192]), ("digits", [4, 11, 24]),
                  ("fills", [4]), ("counts", [4, 3])]
        if len(inputs) != 1 or len(outputs) != 3:
            raise ValueError("wrong model input/output count")
        for actual, (name, shape) in zip([*inputs, *outputs], wanted, strict=True):
            if actual.name != name or actual.type != "tensor(float)" or actual.shape != shape:
                raise ValueError("wrong model tensor contract")

    def read(self, image: Image.Image) -> dict:
        started = time.perf_counter()
        result = _empty_result()
        try:
            if image.mode != "RGB":
                raise ValueError("convert the decoded source frame to RGB before reading")
            pixels = np.asarray(image, dtype=np.uint8)
            admission = inspect_layout(pixels)
            result["status"] = admission
            result["visited"] = True
            result["layout_supported"] = admission == "supported"
            if admission != "supported":
                result["unknown_reason"] = dict.fromkeys(FIELDS, admission)
            else:
                crops = crop_bytes(image)
                x = np.ascontiguousarray(crops.transpose(0, 3, 1, 2), dtype=np.float32) / np.float32(255)
                digits, fills, counts = self.session.run(["digits", "fills", "counts"], {"crops": x})
                for tensor, shape in zip((digits, fills, counts), ((4, 11, 24), (4,), (4, 3)), strict=True):
                    if tensor.dtype != np.float32 or tensor.shape != shape or not np.isfinite(tensor).all():
                        raise ValueError("invalid or nonfinite model output")
                for field, name in enumerate(FIELDS):
                    value = None
                    if field < 2:
                        text = decode_with_count(digits[field], counts[field])
                        bright = _fraction(pixels, CROP_BOXES[field], _white)
                        if 0.015 <= bright <= 0.6 and text:
                            value = int(text)
                        reason = "digit crop lacks visible glyph evidence"
                    else:
                        if 0 <= fills[field] <= 1:
                            value = float(fills[field]) * 100.0
                        reason = "fill outside model domain"
                    result["observations"][name] = value
                    result["known"][name] = value is not None
                    result["unknown_reason"][name] = None if value is not None else reason
                result["status"] = "ok"
        except Exception as error:
            # A transient error is not completed coverage in a progressive scan.
            result["status"] = "error"
            result["visited"] = False
            result["observations"] = dict.fromkeys(FIELDS)
            result["known"] = dict.fromkeys(FIELDS, False)
            result["error"] = str(error)
            result["unknown_reason"] = dict.fromkeys(FIELDS, str(error))
        result["latency_ms"] = (time.perf_counter() - started) * 1000
        return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, default=Path(__file__).with_name("gauge-reader.onnx"))
    parser.add_argument("--image", type=Path, required=True, help="LOCAL full-resolution source frame; never uploaded")
    args = parser.parse_args()
    try:
        reader = GaugeReader(args.model)
        with Image.open(args.image) as image:
            if image.size != (1920, 1080):
                # Do not resize an arbitrary image into an apparently valid HUD.
                result = _empty_result()
                result.update(status="unsupported_geometry", visited=True)
                result["unknown_reason"] = dict.fromkeys(FIELDS, "unsupported_geometry")
            else:
                result = reader.read(image.convert("RGB"))
    except Exception as error:
        result = _empty_result()
        result.update(status="error", error=str(error))
        result["unknown_reason"] = dict.fromkeys(FIELDS, str(error))
    print(json.dumps(result, indent=2, allow_nan=False))
    return 1 if result["status"] == "error" else 0


if __name__ == "__main__":
    raise SystemExit(main())
