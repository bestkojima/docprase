"""对固定教材页执行真实双模型公共 CLI，并核对正文真值、原图定位和资源。"""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'tests/fixtures/ovis/source_page.jpg'
SOURCE_SHA = 'c8cf71eb2f717727dc2d8a3ae5da1e388f6be7bb1e2c4addbde5d40dafb270f6'


def main():
    binary, output = Path(sys.argv[1]), Path(sys.argv[2])
    assert hashlib.sha256(SOURCE.read_bytes()).hexdigest() == SOURCE_SHA
    output.mkdir(parents=True, exist_ok=True)
    command = [str(binary), '--config', 'configs/printed-page.example.json',
               '--input', str(SOURCE), '--out', str(output / 'job')]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=900)
    (output / 'cli.stdout.log').write_text(result.stdout)
    (output / 'cli.stderr.log').write_text(result.stderr)
    job = output / 'job'
    assert result.returncode == 0, (result.returncode, result.stderr)
    document = json.loads((job / 'document.json').read_text())
    manifest = json.loads((job / 'run-manifest.json').read_text())
    import jsonschema
    schema = json.loads((ROOT / 'docs/issue-4/document-ir-1.0.schema.json').read_text())
    jsonschema.validate(document, schema)
    page = document['pages'][0]
    blocks = page['blocks']
    reference = (ROOT / 'tests/fixtures/ovis/chinese_text.reference.txt').read_text().strip()
    matches = [b for b in blocks if b['type'] == 'text' and
               b['content']['text'].strip() == reference and b['status'] == 'ok']
    assert matches, '固定教材正文未被真实双模型识别'
    sample = matches[0]
    expected = [93, 169, 1244, 260]
    actual = sample['bbox']
    intersection = (max(actual[0], expected[0]), max(actual[1], expected[1]),
                    min(actual[2], expected[2]), min(actual[3], expected[3]))
    assert intersection[2] > intersection[0] and intersection[3] > intersection[1]
    assert sample['provenance']['raw_output'].strip() == reference
    assert (job / sample['content']['resource']).read_bytes().startswith(b'\x89PNG')
    assert reference in (job / 'document.md').read_text()
    assert len(manifest['regions']) == len(blocks)
    assert manifest['actual_device'] == 'cpu'
    assert manifest['runtime_configuration']['session_strategy'] == 'shared_model_reset_before_each_region'
    assert manifest['runtime_configuration']['ovis_threads'] == 1
    assert manifest['runtime_configuration']['top_k'] == 40
    assert manifest['runtime_configuration']['temperature'] == 0.8
    assert manifest['runtime_configuration']['seed'] is None
    assert manifest['runtime_configuration']['seed_status'] == 'not_configured'
    prompt = (ROOT / 'tests/fixtures/ovis/prompt.txt').read_bytes().rstrip(b'\n')
    assert manifest['runtime_configuration']['prompt_sha256'] == hashlib.sha256(prompt).hexdigest()
    assert all((job / b['content']['resource']).exists() for b in blocks)
    summary = {'command': command, 'exit_code': result.returncode, 'source_sha256': SOURCE_SHA,
               'status': document['status'], 'blocks': len(blocks),
               'normal_blocks': sum(b['status'] == 'ok' for b in blocks),
               'partial_blocks': sum(b['status'] == 'partial' for b in blocks),
               'truncated_blocks': sum(r['stop_reason'] == 'token_limit' for r in manifest['regions']),
               'matched_text_block': sample['id'], 'matched_text_bbox': actual,
               'matched_text_reference_crop': expected,
               'assets': len(list((job / 'assets').iterdir()))}
    (output / 'summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(summary, ensure_ascii=False))


if __name__ == '__main__':
    main()
