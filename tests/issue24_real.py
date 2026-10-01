"""核验冻结候选的全部真实整页产物及生产JSON重新导出；不再次推理。"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

import jsonschema
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from issue24_run import save, sha


def main():
    run = Path(sys.argv[1]).resolve()
    binary = run / 'candidate/runtime/dococr_cli'
    environment = dict(os.environ, LD_LIBRARY_PATH=str(binary.parent))
    records = {}
    manifest = json.loads((ROOT / 'docs/omnidocbench-20/manifest.json').read_text())
    old = json.loads((ROOT / 'docs/issue-15/samples.json').read_text())
    for group, specs in (('development', manifest['pages']), ('old-seven', old['samples'])):
        for spec in specs:
            key = f"{group}/{spec['id']}"
            job = run / key / 'job'
            errors = []
            assets = {}
            try:
                doc = json.loads((job / 'document.json').read_text())
                schema = ROOT / f"docs/issue-22/document-ir-1.7-{doc['source']['type']}.schema.json"
                jsonschema.validate(doc, json.loads(schema.read_text()))
                for resource in doc['resources']:
                    source = (job / resource['path']).resolve()
                    assert source.is_relative_to(job.resolve()) and source.is_file(), resource['path']
                    with Image.open(source) as image:
                        assert image.size == (resource['width'], resource['height']), resource['path']
                        image.verify()
                    assets[resource['path']] = sha(source)
                with tempfile.TemporaryDirectory(prefix='issue24-reexport-') as directory:
                    target = Path(directory) / 'export'
                    command = [str(binary), '--reexport', str(job / 'document.json'),
                               '--asset-root', str(job), '--out', str(target)]
                    result = subprocess.run(command, cwd=ROOT, env=environment, capture_output=True, text=True)
                    assert result.returncode == 0, result.stderr
                    for name in ('document.json', 'document.md', *assets):
                        assert sha(job / name) == sha(target / name), name
            except (AssertionError, OSError, ValueError, jsonschema.ValidationError) as error:
                errors.append(str(error))
            records[key] = dict(errors=errors, resource_count=len(assets), resources_sha256=assets,
                                document_sha256=sha(job / 'document.json') if (job / 'document.json').exists() else None,
                                markdown_sha256=sha(job / 'document.md') if (job / 'document.md').exists() else None)
            print(key, 'passed' if not errors else errors, flush=True)
    failures = {key: value['errors'] for key, value in records.items() if value['errors']}
    save(run / 'real-export-verification.json', dict(records=records, failures=failures,
         jobs_expected=26, jobs_checked=len(records), pages_expected=27, passed=not failures and len(records) == 26))
    return int(bool(failures))


if __name__ == '__main__':
    raise SystemExit(main())
