"""Summarize measured per-command Linux resources and manifest stages."""
import json
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]


def seconds(value):
    pieces = value.split(':')
    return sum(float(piece) * 60**i for i, piece in enumerate(reversed(pieces)))


def main():
    output = Path(sys.argv[1]).resolve()
    samples = json.loads((ROOT / 'docs/issue-15/samples.json').read_text())['samples']
    report = {'measurement': 'GNU /usr/bin/time -v wait4 max RSS; fresh CLI processes; OS page cache not cleared',
              'samples': {}}
    for sample in samples:
        folder = output / sample['id']
        if not (folder / 'job/run-manifest.json').exists():
            continue
        time_output = (folder / 'time.txt').read_text()
        def field(pattern):
            match = re.search(pattern, time_output, re.M)
            return match.group(1) if match else None
        manifest = json.loads((folder / 'job/run-manifest.json').read_text())
        row = {'elapsed_wall_seconds': seconds(field(r'^\s*Elapsed \(wall clock\) time \(h:mm:ss or m:ss\):\s*(\S+)')),
               'max_rss_kib': int(field(r'^\s*Maximum resident set size \(kbytes\):\s*(\d+)')),
               'user_cpu_seconds': float(field(r'^\s*User time \(seconds\):\s*(\S+)')),
               'system_cpu_seconds': float(field(r'^\s*System time \(seconds\):\s*(\S+)')),
               'manifest_timings_ms': manifest['timings_ms'],
               'manifest_timing_status': manifest['timing_status'],
               'actual_backend': manifest['actual_backend'],
               'actual_device': manifest['actual_device'],
               'runtime_version': manifest['runtime_version'],
               'artifacts': manifest['artifacts'],
               'processing': manifest['processing'],
               'manifest_peak_memory': manifest['metrics']['peak_memory_bytes']}
        if 'pdf' in manifest:
            row['pdf'] = manifest['pdf']
        report['samples'][sample['id']] = row
    (output / 'performance.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({name: {'wall_s': row['elapsed_wall_seconds'],
                             'max_rss_kib': row['max_rss_kib']}
                      for name, row in report['samples'].items()}, ensure_ascii=False))


if __name__ == '__main__':
    main()
