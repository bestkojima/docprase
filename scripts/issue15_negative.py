"""Exercise production CLI rejection contracts without loading model for bad config."""
import copy
import json
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]


def main():
    cli = Path(sys.argv[1]).resolve()
    out = Path(sys.argv[2]).resolve()
    out.mkdir(parents=True, exist_ok=False)
    config = json.loads((ROOT / 'configs/printed-page.example.json').read_text())
    source = ROOT / 'tests/fixtures/ovis/source_page.jpg'
    cases = []
    for name, change, marker in [
        ('unknown_processor', lambda x: x['flow'][0].update(processor='imaginary'),
         'unknown_processor'),
        ('duplicate_processing', lambda x: x['processing'].append(copy.deepcopy(x['processing'][2])),
         'duplicate_processing'),
        ('required_processing_disabled', lambda x: x['processing'][0].update(enabled=False),
         'required_processing_disabled'),
        ('processing_owner_mismatch', lambda x: x['processing'][2].update(owner='runtime'),
         'processing_owner_mismatch'),
        ('wrong_artifact_hash', lambda x: x['models']['layout']['artifacts'][0].update(sha256='0'*64),
         'artifact_hash_mismatch'),
        ('unverified_contract', lambda x: x['models']['layout'].update(contract_status='pending_probe'),
         'contract_unverified'),
        ('token_budget_too_large', lambda x: x['execution'].update(max_new_tokens=4097),
         'unsupported_parameter'),
    ]:
        bad = copy.deepcopy(config)
        change(bad)
        path = out / f'{name}.json'
        path.write_text(json.dumps(bad, ensure_ascii=False, indent=2) + '\n')
        cases.append((name, path, source, marker))
    cases.extend([
        ('encrypted_pdf', ROOT / 'configs/printed-page.example.json',
         ROOT / 'tests/fixtures/encrypted.pdf', 'encrypted'),
        ('invalid_image', ROOT / 'configs/printed-page.example.json',
         ROOT / 'tests/fixtures/ovis/invalid.png', 'input_error'),
    ])
    report = []
    for name, cfg, input_path, marker in cases:
        cmd = [str(cli), '--config', str(cfg), '--input', str(input_path),
               '--out', str(out / f'{name}-job')]
        result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=90)
        (out / f'{name}.stdout.log').write_text(result.stdout)
        (out / f'{name}.stderr.log').write_text(result.stderr)
        observed = {'case': name, 'command': cmd, 'returncode': result.returncode,
                    'expected_marker': marker, 'marker_found': marker in result.stderr if marker else None,
                    'stderr': result.stderr}
        observed['pass'] = result.returncode != 0 and (marker is None or observed['marker_found'])
        report.append(observed)
        print(name, result.returncode, observed['pass'], flush=True)
    (out / 'summary.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    return int(not all(row['pass'] for row in report))


if __name__ == '__main__':
    sys.exit(main())
