"""以公共作业回放开发集原始输出；fixture 不代表真实视觉成功。"""
import argparse
from collections import Counter
import hashlib
import json
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from cli_integration import png_2x2
from printed_page_integration import config


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture-cli', type=Path, required=True)
    parser.add_argument('--corpus', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    binary = args.fixture_cli.resolve()
    corpus = args.corpus.resolve()
    records, missing = [], []
    with tempfile.TemporaryDirectory(prefix='dococr-output-audit-') as temporary:
        root = Path(temporary)
        image, setting, fixture = root / 'page.png', root / 'config.json', root / 'output.json'
        image.write_bytes(png_2x2())
        setting.write_text(json.dumps(config('printed_page_quality')))
        env = dict(os.environ, DOCOCR_TEST_OUTPUT_PATH=str(fixture))
        for index in range(1, 21):
            source = corpus / f'odb-{index:02d}' / 'job' / 'document.json'
            if not source.is_file():
                missing.append(f'odb-{index:02d}')
                continue
            document = json.loads(source.read_text())
            manifest = json.loads(source.with_name('run-manifest.json').read_text())
            regions = {r['request_id']: r for r in manifest['regions']}
            source_hash = hashlib.sha256(source.read_bytes()).hexdigest()
            for page in document['pages']:
                for block in page['blocks']:
                    raw = block['provenance']['raw_output']
                    if block['type'] != 'text' or not raw:
                        continue
                    stop = regions[block['provenance']['request_id']]['stop_reason']
                    output = dict(text=raw, finish_reason='truncated' if stop == 'token_limit' else
                        'failed' if block['status'] == 'failed' else 'complete', stop_reason=stop)
                    fixture.write_text(json.dumps(output))
                    target = root / str(len(records))
                    process = subprocess.run([str(binary), '--config', str(setting), '--input',
                        str(image), '--out', str(target)], cwd=ROOT, env=env,
                        capture_output=True, text=True, timeout=30)
                    assert process.returncode == 0, process.stderr
                    result = json.loads((target / 'document.json').read_text())['pages'][0]['blocks'][0]
                    assessment = result['provenance']['assessment']
                    generation_text = assessment.pop('generation_text')
                    assessment['generation_text_sha256'] = hashlib.sha256(generation_text.encode()).hexdigest()
                    records.append(dict(sample=f'odb-{index:02d}', block_id=block['id'],
                        source_document_sha256=source_hash, original_status=block['status'],
                        raw_sha256=hashlib.sha256(raw.encode()).hexdigest(), raw_bytes=len(raw.encode()),
                        assessment=assessment))
                    shutil.rmtree(target)
            print(f'odb-{index:02d}: {len(records)} text outputs audited', flush=True)
    report = dict(policy='region-output-v1', evidence_kind='public_job_fixture_replay_of_real_raw_outputs',
        corpus=str(args.corpus), available_pages=20-len(missing), missing_pages=missing,
        counts=dict(Counter(r['assessment']['state'] for r in records)),
        previously_ok_counts=dict(Counter(r['assessment']['state'] for r in records if r['original_status']=='ok')),
        records=records)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k != 'records'}, ensure_ascii=False))


if __name__ == '__main__':
    main()
