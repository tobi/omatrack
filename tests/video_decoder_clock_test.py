"""Finite real-codec PTS/origin/seek and RGB regression for the native decoder.

Uses generated private working files only. Run with uv and PyAV; the C++ decoder
probe is built by the normal integration build. No GUI/model/source data needed.
"""
from __future__ import annotations
import argparse
from bisect import bisect_left
from fractions import Fraction
import json
from pathlib import Path
import subprocess

import av
import numpy as np


def generate(path: Path, codec: str, offset_ms: int):
    ticks = [0, 17, 54, 134, 150, 250, 300, 337, 437, 537, 570, 670, 720]
    with av.open(str(path), 'w') as c:
        s = c.add_stream(codec, rate=30)
        s.width, s.height = 96, 64
        s.pix_fmt = 'bgr0' if codec == 'ffv1' else 'yuv420p'
        s.time_base = Fraction(1, 1000)
        s.codec_context.time_base = Fraction(1, 1000)
        if codec == 'libx264':
            s.options = {'preset': 'medium', 'crf': '10', 'bf': '3', 'g': '12'}
            # Regression: ignoring declared BT.709/range changes real HUD pixels.
            s.codec_context.colorspace = 1  # AVCOL_SPC_BT709
            s.codec_context.color_range = 1  # AVCOL_RANGE_MPEG
        for i, t in enumerate(ticks):
            image = np.zeros((64, 96, 3), dtype=np.uint8)
            image[:] = [25+i*14, 215-i*12, 40+i*9]
            frame = av.VideoFrame.from_ndarray(image, format='rgb24')
            if codec == 'libx264':
                frame.colorspace = 1
                frame.color_range = 1
            frame.pts = offset_ms+t
            frame.time_base = Fraction(1, 1000)
            for packet in s.encode(frame): c.mux(packet)
        for packet in s.encode(None): c.mux(packet)


def check(path: Path, binary: Path):
    with av.open(str(path)) as c:
        origin_ns = int(c.start_time*1000)
        rows = [{'source_pts_ns': int(f.pts*f.time_base*1_000_000_000),
                 'center_rgb': f.to_ndarray(format='rgb24')[32,48].tolist()}
                for f in c.decode(video=0)]
    times = [r['source_pts_ns']-origin_ns for r in rows]
    assert len(set(np.diff(times).tolist())) > 1, 'muxer regularized fixture PTS'
    queries = [0, 18_000_000, 134_000_000, 536_000_000, 250_000_000, 0, 670_000_000]
    result = subprocess.run([str(binary), str(path), *[str(q/1e9) for q in queries]],
                            check=True, capture_output=True, text=True, timeout=20)
    actual = [json.loads(line) for line in result.stdout.splitlines()]
    assert len(actual) == len(queries)
    for query, row in zip(queries, actual, strict=True):
        idx = bisect_left(times, query)
        assert row['pts_ns'] == times[idx], (query, row, times[idx])
        assert row['source_pts_ns'] == rows[idx]['source_pts_ns']
        assert row['origin_ns'] == origin_ns
        assert row['center_rgb'] == rows[idx]['center_rgb'], (query,row,rows[idx])
        assert row['metadata'] is False
    return {'file': path.name, 'origin_ns': origin_ns, 'native_frames': len(rows),
            'native_deltas_ns': sorted(set(np.diff(times).tolist())),
            'queries': len(queries), 'pts_exact': True, 'rgb_exact': True}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--decoder',type=Path,required=True)
    p.add_argument('--output-dir',type=Path,required=True)
    a=p.parse_args();a.output_dir.mkdir(parents=True,exist_ok=True)
    if any(a.output_dir.iterdir()): raise ValueError('output directory must be empty')
    reports=[]
    for name,codec,origin in [('vfr-zero.mkv','ffv1',0),('vfr-offset.mkv','ffv1',3000),
                              ('bframes-offset.mp4','libx264',3000)]:
        path=a.output_dir/name;generate(path,codec,origin);reports.append(check(path,a.decoder.resolve()))
    (a.output_dir/'receipt.json').write_text(json.dumps(reports,indent=2)+'\n')
    print(json.dumps(reports,indent=2))


if __name__=='__main__': main()
