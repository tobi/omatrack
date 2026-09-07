"""Generated display-matrix rejection fixtures; run with uv, PyAV and FFmpeg >=6.
No private images, model, GUI or original recording writes. This checks geometry,
not color parity (the separate clock test owns that independent assertion).
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from video_decoder_clock_test import generate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--decoder', type=Path, required=True)
    parser.add_argument('--ffmpeg', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=False)
    source = args.output_dir / 'untransformed.mp4'
    generate(source, 'libx264', 0)
    before = hashlib.sha256(source.read_bytes()).hexdigest()
    decoder = str(args.decoder.resolve())
    subprocess.run([decoder, str(source), '0'], check=True, capture_output=True, timeout=20)
    reports = []
    transforms = [('rotate-90', ['-display_rotation', '90']),
                  ('rotate-180', ['-display_rotation', '180']),
                  ('mirror-horizontal', ['-display_hflip']),
                  ('mirror-vertical', ['-display_vflip'])]
    for name, options in transforms:
        rotated = args.output_dir / f'display-{name}.mp4'
        subprocess.run([str(args.ffmpeg.resolve()), '-nostdin', '-v', 'error',
                        *options, '-i', str(source),
                        '-map', '0:v:0', '-c', 'copy', str(rotated)],
                       check=True, timeout=20)
        result = subprocess.run([decoder, str(rotated), '0'],
                                capture_output=True, text=True, timeout=20)
        assert result.returncode != 0 and 'display transform' in result.stderr, result
        reports.append({'transform': name, 'rejected': True})
    assert before == hashlib.sha256(source.read_bytes()).hexdigest()
    (args.output_dir / 'receipt.json').write_text(json.dumps(reports, indent=2) + '\n')
    print(json.dumps(reports))


if __name__ == '__main__':
    main()
