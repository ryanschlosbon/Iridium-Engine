"""M9 temporal-AA evaluation against supersampled references (harness tooling only).

For each fixture, scores scene-linear captures with IridiumTemporalMetrics:
  - reference error of the no-AA single frame and of a converged TAA frame
    against the held accumulation reference (scene domain, at the fixture's
    display exposure, read from the capture sidecar);
  - static stability of a TAA sequence (frame-to-frame delta, temporal std,
    flicker energy).

usage:
  python tools/m9/Evaluate-Temporal.py --reference-label ref64-h150 \
      --candidate-label taa-v0-h150 [--baseline-label noaa-h150] [--out report.json]
"""
import argparse
import glob
import json
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
CAPTURES = os.path.join(ROOT, 'out', 'm9', 'captures')
METRICS = os.path.join(ROOT, 'out', 'build', 'x64-release', 'bin', 'IridiumTemporalMetrics.exe')


def frames(label, fixture, point):
    return sorted(glob.glob(os.path.join(CAPTURES, label, fixture, point, '*.pfm')))


def exposure_ev(pfm):
    sidecar = os.path.splitext(pfm)[0] + '.json'
    try:
        with open(sidecar, encoding='utf-8-sig') as stream:
            meta = json.load(stream)
        return float(meta['render_configuration'].get('manual_exposure_ev', 0.0))
    except (OSError, KeyError, ValueError):
        return 0.0


def run(args):
    result = subprocess.run([METRICS] + args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f'{args[0]} failed: {result.stderr.strip()}')
    return json.loads(result.stdout)


def pick(report, keys):
    out = {}
    def walk(node, path=''):
        if isinstance(node, dict):
            for key, value in node.items():
                walk(value, f'{path}.{key}' if path else key)
        elif path.split('.')[-1] in keys and isinstance(node, (int, float, str)):
            out[path.split('.')[-1]] = node
    walk(report)
    return out


ERROR_KEYS = {'log2_luminance_mae', 'log2_luminance_rmse', 'tone_mapped_rmse',
              'above_threshold_fraction', 'luminance_rmse', 'max_abs_tone_mapped'}
STABILITY_KEYS = {'mean_frame_delta', 'p99_frame_delta', 'mean_temporal_std',
                  'flicker_pixel_fraction', 'flicker_energy'}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--reference-label', required=True)
    parser.add_argument('--candidate-label', required=True)
    parser.add_argument('--baseline-label')
    parser.add_argument('--candidate-point', default='scene-resolved')
    parser.add_argument('--out')
    options = parser.parse_args()
    report = {}
    for ref_dir in sorted(glob.glob(os.path.join(CAPTURES, options.reference_label, '*', 'reference'))):
        fixture = os.path.basename(os.path.dirname(ref_dir))
        refs = glob.glob(os.path.join(ref_dir, '*__ref*.pfm'))
        if not refs:
            continue
        reference = refs[0]
        candidates = frames(options.candidate_label, fixture, options.candidate_point)
        if not candidates:
            continue
        ev = exposure_ev(candidates[-1])
        entry = {'exposure_ev': ev}
        common = ['--domain', 'scene', '--exposure-ev', str(ev)]
        entry['taa_error'] = pick(run(['reference-error', '--reference', reference,
                                       '--test', candidates[-1]] + common), ERROR_KEYS)
        if len(candidates) > 1:
            entry['taa_stability'] = pick(run(['stability', '--frames'] + candidates + common),
                                          STABILITY_KEYS)
        if options.baseline_label:
            baseline = frames(options.baseline_label, fixture, 'scene')
            if baseline:
                entry['noaa_error'] = pick(run(['reference-error', '--reference', reference,
                                                '--test', baseline[-1]] + common), ERROR_KEYS)
        report[fixture] = entry
    text = json.dumps(report, indent=1, sort_keys=True)
    if options.out:
        with open(options.out, 'w', encoding='utf-8') as stream:
            stream.write(text)
    print(text)


if __name__ == '__main__':
    sys.exit(main())
