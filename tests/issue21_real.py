"""真实双模型公共作业：已知小裁图、异常展示、资源与重新导出。"""
from collections import Counter
import hashlib
import json
from pathlib import Path
import subprocess
import sys

from PIL import Image
import jsonschema
from printed_page_integration import ROOT


def main():
    binary, output = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    output.mkdir(parents=True, exist_ok=True)
    records = []
    samples = [('caption', 'tests/fixtures/issue15/printed_science_2col.png',
                'Figure 1. Recorded readings.'),
               ('header', 'tests/fixtures/layout/exam-jee-346.jpg', 'JEE (Advanced) 2023')]
    for name, source_path, expected in samples:
        saved = output / name
        command = [str(binary), '--config', 'configs/printed-page.example.json',
                   '--input', source_path, '--out', str(saved)]
        process = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=600)
        (output / f'{name}.stdout.log').write_text(process.stdout)
        (output / f'{name}.stderr.log').write_text(process.stderr)
        assert process.returncode == 0, process.stderr
        doc = json.loads((saved / 'document.json').read_text())
        jsonschema.validate(doc, json.loads((ROOT / 'schemas/document-ir/document-ir-1.10-image.schema.json').read_text()))
        blocks = doc['pages'][0]['blocks']
        known = [b for b in blocks if b['provenance']['raw_output'].strip() == expected]
        assert len(known) == 1 and known[0]['status'] == 'ok', known
        assert known[0]['provenance']['visual']['token_count'] > 0
        assert known[0]['provenance']['assessment']['state'] == 'ok'
        markdown = (saved / 'document.md').read_text()
        assert expected in markdown
        for resource in doc['resources']:
            with Image.open(saved / resource['path']) as image:
                assert image.size == (resource['width'], resource['height'])
        unreliable = []
        for block in blocks:
            assessment = block['provenance']['assessment']
            if block['type'] in ('text', 'formula', 'table') and assessment['state'] != 'ok':
                assert f"![原图]({block['content']['resource']})" in markdown
                raw = block['provenance']['raw_output']
                if assessment['state'] == 'anomalous' and raw:
                    assert raw not in markdown
                unreliable.append(dict(id=block['id'], raw_output=raw,
                    assessment=assessment, resource=block['content']['resource']))
        reexport = output / f'{name}-reexport'
        process = subprocess.run([str(binary), '--reexport', str(saved / 'document.json'),
            '--asset-root', str(saved), '--out', str(reexport)], cwd=ROOT, capture_output=True, text=True)
        assert process.returncode == 0, process.stderr
        assert (reexport / 'document.json').read_bytes() == (saved / 'document.json').read_bytes()
        assert (reexport / 'document.md').read_bytes() == (saved / 'document.md').read_bytes()
        for asset_path in saved.joinpath('assets').iterdir():
            assert (reexport / 'assets' / asset_path.name).read_bytes() == asset_path.read_bytes()
        record = dict(name=name, source=source_path, source_sha256=hashlib.sha256((ROOT / source_path).read_bytes()).hexdigest(),
            document_sha256=hashlib.sha256((saved / 'document.json').read_bytes()).hexdigest(),
            status=doc['status'], counts=dict(Counter(b['provenance']['assessment']['state'] for b in blocks)),
            resources=len(doc['resources']), known_block=known[0], unreliable=unreliable,
            reexport_identical=True, command=command)
        records.append(record)
        print(f'{name}: {record["counts"]}; known crop matched; reexport identical', flush=True)
    (output / 'summary.json').write_text(json.dumps(records, ensure_ascii=False, indent=2)+'\n')


if __name__ == '__main__':
    main()
