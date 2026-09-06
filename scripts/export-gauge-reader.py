#!/usr/bin/env -S uv run --script
"""Export the selected quarantine-v3 CountReader, offline and without training.

Use the pinned project-local uv environment described in docs/GAUGE_READER_RUNTIME.md.
No model-zoo weights, inputs, or outputs are uploaded. The research checkout is read-only.
"""
from __future__ import annotations

import argparse
import ast
import hashlib
import importlib.metadata
import json
import math
import os
from pathlib import Path
import subprocess
import sys

# Set these BEFORE importing research code. Never create __pycache__ in that checkout.
sys.dont_write_bytecode = True
os.environ.setdefault("HF_HUB_OFFLINE", "1")
os.environ.setdefault("HF_DATASETS_OFFLINE", "1")

CHECKPOINT_SHA = "2b1bedae45f08c9187e8e26a92cc1a01db30bda812e8fb7d85fe51368a80faeb"
SOURCE_SHA = {
    "model.py": "4fa35987a176f12440ff06a6fd9715cdca1c1aac9040ba182bde4a76ec6109e8",
    "count_model.py": "b8741d14fb44a129822572144e39cf899dfd55a9f23342a436f7ed556f583ca6",
    "decode.py": "535fe8fd47fab88c6a2cf4de998f19215bde84418eb63b8373884d79db30aa77",
    "data.py": "21b5a3bce895b5767a7cc57515381d91a1f3b8d8a1a83c0cfab84555f6cbc68b",
}
CONTRACT = "omatrack-crop-count-v1"
PREPROCESSING = "pillow-rgb-bilinear-22bit-crop-count-v1"


def sha(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def crop_functions(source: Path, np, Image):
    """Load the EXACT checked crop functions, without importing training datasets.

    data.py imports legacy trainer modules at module scope. Select only its
    image constants and two pure functions via AST; do not rewrite their bodies,
    monkeypatch training modules, import records, or access dataset labels.
    """
    tree = ast.parse(source.read_text(), filename=str(source))
    selected = []
    for node in tree.body:
        if isinstance(node, ast.FunctionDef) and node.name in {"crop", "tensor_crops"}:
            selected.append(node)
        if isinstance(node, ast.Assign) and any(
            isinstance(t, ast.Name) and t.id in {"FIELDS", "SIZE", "LAYOUTS"} for t in node.targets
        ):
            selected.append(node)
    namespace = {"np": np, "Image": Image, "math": math}
    exec(compile(ast.Module(body=selected, type_ignores=[]), str(source), "exec"), namespace)
    return namespace["tensor_crops"], namespace["LAYOUTS"]["tds_aim_orange"]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--research-dir", type=Path, required=True)
    parser.add_argument("--checkpoint", type=Path, help="Defaults to the selected checkpoint in research-dir")
    parser.add_argument("--output-dir", type=Path, required=True, help="Private, ignored output directory")
    parser.add_argument("--image", type=Path, action="append", default=[], help="Real full-resolution RGB validation image; repeatable")
    parser.add_argument("--image-dir", type=Path, help="Sorted PNG/JPEG images to validate (no recursive scan)")
    parser.add_argument("--native-test", type=Path, help="Optional compiled gauge_reader_test executable; must pass before publishing model")
    args = parser.parse_args()
    research = args.research_dir.resolve()
    checkpoint = (args.checkpoint or research / "work/crop_reader_quarantine_v3/training/selected.pt").resolve()
    output = args.output_dir.resolve()
    if output == research or research in output.parents:
        parser.error("output-dir must not modify the research checkout")
    if sha(checkpoint) != CHECKPOINT_SHA:
        parser.error("checkpoint SHA256 is not the selected quarantine-v3 checkpoint; refusing substitution")
    code = research / "runs/crop_reader"
    for name, digest in SOURCE_SHA.items():
        if sha(code / name) != digest:
            parser.error(f"reviewed research source changed: {name}; review rather than silently exporting")
    images = list(args.image)
    if args.image_dir:
        images += sorted(p for p in args.image_dir.iterdir() if p.suffix.lower() in {".png", ".jpg", ".jpeg"})
    if not images:
        parser.error("supply real local full-resolution validation images; random-only parity is insufficient")
    if any(output == p.resolve() or output in p.resolve().parents for p in [checkpoint, *images]):
        parser.error("input files must be outside output-dir; never overwrite evidence")
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        parser.error("output-dir must be empty; choose a new directory for a reproducible comparison")
    final_model = output / "gauge-reader.onnx"

    import numpy as np
    import onnx
    import onnxruntime as ort
    from PIL import Image
    import torch

    torch.set_num_threads(1)
    torch.manual_seed(0)
    torch.use_deterministic_algorithms(True)
    sys.path.insert(0, str(research))
    from runs.crop_reader.count_model import CountReader, decode_with_count

    tensor_crops, layout = crop_functions(code / "data.py", np, Image)
    payload = torch.load(checkpoint, map_location="cpu", weights_only=True)
    if not {"model", "config", "epoch"} <= set(payload):
        raise ValueError("checkpoint lacks expected provenance")
    model = CountReader(pretrained=False).cpu().eval()
    model.load_state_dict(payload["model"], strict=True)
    if sum(p.numel() for p in model.parameters()) != 551783:
        raise ValueError("unexpected model parameter count")
    temporary_model = output / "gauge-reader.partial.onnx"
    with torch.inference_mode():
        torch.onnx.export(
            model, torch.zeros(4, 3, 64, 192), str(temporary_model),
            input_names=["crops"], output_names=["digits", "fills", "counts"],
            opset_version=17, dynamo=False, external_data=False,
            do_constant_folding=True, export_params=True,
        )
    graph = onnx.load(temporary_model)
    onnx.helper.set_model_props(graph, {
        "omatrack.contract": CONTRACT,
        "omatrack.checkpoint_sha256": CHECKPOINT_SHA,
        "omatrack.preprocessing": PREPROCESSING,
        "omatrack.layout": "tds_aim_orange-1920x1080",
        "omatrack.decoder": "count-argmax-plus-one-ctc-prefix-beam-10-per-length",
        "omatrack.fields": "gear,stint_lap,brake_fill_pct,throttle_fill_pct",
    })
    onnx.checker.check_model(graph, full_check=True)
    onnx.save_model(graph, temporary_model, save_as_external_data=False)
    options = ort.SessionOptions()
    options.intra_op_num_threads = 1
    options.inter_op_num_threads = 1
    session = ort.InferenceSession(str(temporary_model), sess_options=options, providers=["CPUExecutionProvider"])
    fixtures = output / "fixtures"
    fixtures.mkdir(exist_ok=True)
    (fixtures / "malformed.onnx").write_bytes(b"not an ONNX model\n")
    # Negative schema/provenance fixtures: same network is NOT admissible without
    # the current metadata, nor with a substituted checkpoint identity.
    onnx.helper.set_model_props(graph, {})
    onnx.save_model(graph, fixtures / "unversioned.onnx", save_as_external_data=False)
    graph = onnx.load(temporary_model)
    for entry in graph.metadata_props:
        if entry.key == "omatrack.checkpoint_sha256":
            entry.value = "0" * 64
    onnx.save_model(graph, fixtures / "wrong-checkpoint.onnx", save_as_external_data=False)
    valid_graph = onnx.load(temporary_model)
    constants = [np.zeros((4, 11, 24), dtype=np.float32),
                 np.array([0, 0, np.nan, 1], dtype=np.float32),
                 np.zeros((4, 3), dtype=np.float32)]
    nodes = [onnx.helper.make_node("Constant", [], [name], value=onnx.numpy_helper.from_array(value))
             for name, value in zip(["digits", "fills", "counts"], constants, strict=True)]
    invalid_values = onnx.helper.make_model(
        onnx.helper.make_graph(nodes, "nonfinite-test", list(valid_graph.graph.input), list(valid_graph.graph.output)),
        opset_imports=list(valid_graph.opset_import), ir_version=valid_graph.ir_version,
    )
    onnx.helper.set_model_props(invalid_values, {p.key: p.value for p in valid_graph.metadata_props})
    onnx.save_model(invalid_values, fixtures / "nonfinite.onnx", save_as_external_data=False)
    expected = []
    image_receipts = []
    max_errors = np.zeros(3)
    with torch.inference_mode():
        for index, path in enumerate(images):
            with Image.open(path) as image:
                if image.size != (1920, 1080):
                    raise ValueError(f"validation image {path} is not native 1920x1080")
                image = image.convert("RGB")
                crops = tensor_crops(image, layout)
                basename = f"frame-{index:03d}"
                image.save(fixtures / f"{basename}.ppm")
            crops.tofile(fixtures / f"{basename}.crops") # exact uint8 NHWC preprocessing oracle
            x = torch.from_numpy(crops.copy()).permute(0, 3, 1, 2).float() / 255
            reference = model(x)
            actual = session.run(None, {"crops": x.numpy()})
            for i, (a, b) in enumerate(zip(actual, reference, strict=True)):
                delta = float(np.max(np.abs(a - b.numpy())))
                max_errors[i] = max(max_errors[i], delta)
                np.testing.assert_allclose(a, b.numpy(), rtol=3e-4, atol=3e-4)
            strings = decode_with_count(reference[0][:2], reference[2][:2])
            onnx_strings = decode_with_count(torch.from_numpy(actual[0][:2]), torch.from_numpy(actual[2][:2]))
            if strings != onnx_strings:
                raise AssertionError(f"count-constrained CTC changed on {path}")
            values = [int(strings[0]), int(strings[1]), float(reference[1][2]) * 100, float(reference[1][3]) * 100]
            expected.append("\t".join([basename, *(str(v) for v in values)]))
            image_receipts.append({"image_sha256": sha(path), "fixture": basename, "values": values})
    (fixtures / "expected.tsv").write_text("\n".join(expected) + "\n")

    # Decoder oracles cover raw real logits as well as non-greedy, repeated-digit,
    # 1/2/3-digit-count and tie cases. These do not replace the real-frame parity.
    rng = np.random.default_rng(20260905)
    decoder_rows = []
    for i in range(36):
        logits = rng.normal(0, 4, (1, 11, 24)).astype(np.float32)
        counts = np.full((1, 3), -4, dtype=np.float32)
        counts[0, i % 3] = 4
        if i < 3:
            logits.fill(0) # stable tie behavior
        elif i < 6:
            logits.fill(-12)
            logits[0, 2, :] = 6 # digit '1', same-count repetitions need blanks
            logits[0, 0, 5::6] = 10
        text = decode_with_count(torch.from_numpy(logits), torch.from_numpy(counts))[0]
        name = f"decoder-{i:03d}"
        np.concatenate([logits.ravel(), counts.ravel()]).astype("<f4").tofile(fixtures / f"{name}.f32")
        decoder_rows.append(f"{name}\t{text}")
    (fixtures / "decoder.tsv").write_text("\n".join(decoder_rows) + "\n")

    # Native tests can read the staged model; publish only after all checks pass.
    if args.native_test:
        subprocess.run([str(args.native_test.resolve()), "--model", str(temporary_model), "--fixtures", str(fixtures)], check=True)
    receipt = {
        "contract": CONTRACT, "checkpoint_sha256": CHECKPOINT_SHA,
        "epoch_zero_based": payload["epoch"], "parameters": 551783,
        "onnx_sha256": sha(temporary_model), "opset": 17,
        "input": [4, 3, 64, 192], "outputs": [[4, 11, 24], [4], [4, 3]],
        "preprocessing": PREPROCESSING, "research_source_sha256": SOURCE_SHA,
        "exporter_sha256": sha(Path(__file__)),
        "versions": {name: importlib.metadata.version(name) for name in ["torch", "torchvision", "timm", "pillow", "numpy", "onnx", "onnxruntime"]},
        "parity_max_abs_error": dict(zip(["digits", "fills", "counts"], max_errors.tolist(), strict=True)),
        "real_images": image_receipts, "native_test_passed": bool(args.native_test),
        "training": False, "native_telemetry_used": False,
        "accuracy_claim": "PyTorch/export/native parity, not independent pixel accuracy or generic HUD detection",
    }
    temporary_model.replace(final_model)
    (output / "receipt.json").write_text(json.dumps(receipt, indent=2, allow_nan=False) + "\n")
    print(json.dumps({"model": str(final_model), "sha256": receipt["onnx_sha256"], "images": len(images), "parity": receipt["parity_max_abs_error"]}, indent=2))


if __name__ == "__main__":
    main()
