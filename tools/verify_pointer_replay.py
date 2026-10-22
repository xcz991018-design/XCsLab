"""Offline regression against exported historical configuration (no DB writes).

Requires Python OpenCV. Example:
  python tools/verify_pointer_replay.py --snapshot build/diagnostics/05ea-before/snapshot.json \
    --image build/diagnostics/05ea-before/original.jpg --angle 193.6 --value 0.202
"""
import argparse
import json
import subprocess
from pathlib import Path

import cv2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', default='build/Release/gauge_reader_service.exe')
    parser.add_argument('--snapshot', required=True)
    parser.add_argument('--image', required=True)
    parser.add_argument('--angle', type=float, required=True)
    parser.add_argument('--value', type=float, required=True)
    parser.add_argument('--angle-tolerance', type=float, default=3.0)
    parser.add_argument('--value-tolerance', type=float, default=0.015)
    parser.add_argument('--output', default='build/diagnostics/pointer-regression')
    args = parser.parse_args()
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    original = Path(args.image).resolve()
    frame = cv2.imread(str(original))
    if frame is None:
        raise RuntimeError('Cannot decode input image')
    variants = {'original': original}
    for name, image in {
        'compressed': frame,
        'darker': cv2.convertScaleAbs(frame, alpha=0.8),
        'brighter': cv2.convertScaleAbs(frame, alpha=1.15, beta=5),
        'blur': cv2.GaussianBlur(frame, (3, 3), 0.7),
    }.items():
        variants[name] = output / (name + '.jpg')
        if not cv2.imwrite(str(variants[name]), image, [cv2.IMWRITE_JPEG_QUALITY, 85]):
            raise RuntimeError('Cannot write test image')
    rows = []
    for name, image in variants.items():
        mode_results = []
        for mode in ('production', 'evidence', 'debug'):
            destination = output / (name + '-' + mode)
            completed = subprocess.run([
                str(Path(args.exe).resolve()), '--diagnose-snapshot',
                str(Path(args.snapshot).resolve()), '--image', str(image),
                '--mode', mode, '--output', str(destination),
            ], capture_output=True)
            if completed.returncode:
                raise RuntimeError(completed.stderr.decode(errors='replace'))
            report = json.loads((destination / 'report.json').read_text(encoding='utf-8'))
            angle_error = abs((report['angle'] - args.angle + 180) % 360 - 180)
            if (not report['success'] or angle_error > args.angle_tolerance or
                    abs(report['value'] - args.value) > args.value_tolerance):
                raise AssertionError(f'{name}/{mode}: {report}')
            mode_results.append((report['angle'], report['value']))
            rows.append(dict(variant=name, mode=mode, angle=report['angle'], value=report['value']))
        if len(set(mode_results)) != 1:
            raise AssertionError(f'Run modes disagree: {name}: {mode_results}')
    (output / 'summary.json').write_text(json.dumps(rows, indent=2), encoding='utf-8')
    print(f'PASS: {len(rows)} replays; all modes agree; values '
          f'{min(r["value"] for r in rows):.6f}..{max(r["value"] for r in rows):.6f}')


if __name__ == '__main__':
    main()
