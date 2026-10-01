"""从无损原始包验证12页真实产物、图片资源及冻结生产 CLI 重新导出。"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

import jsonschema
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from issue25_materials import frozen_runtime, read_frozen, restore_page
from issue24_run import candidate_intact, save, sha


def main():
    run = Path(sys.argv[1]).resolve()
    record, manifest, _ = read_frozen(run, Path(__file__))
    candidate = json.loads((run / 'freeze/docs/issue-24/evidence/candidate.json').read_text())
    candidate_run = Path(record['candidate_run'])
    binary, _, environment = frozen_runtime(candidate_run, candidate)
    records = {}
    for spec in manifest['pages']:
        errors, resources = [], {}
        document_sha = None
        try:
            with restore_page(run, spec['id']) as folder:
                job = folder / 'job'
                document = json.loads((job / 'document.json').read_text())
                document_sha = sha(job / 'document.json')
                schema = ROOT / f"docs/issue-22/document-ir-1.7-{document['source']['type']}.schema.json"
                jsonschema.validate(document, json.loads(schema.read_text()))
                for resource in document['resources']:
                    image_path = (job / resource['path']).resolve()
                    assert image_path.is_relative_to(job.resolve()) and image_path.is_file(), resource['path']
                    with Image.open(image_path) as image:
                        assert image.size == (resource['width'], resource['height']), resource['path']
                        image.verify()
                    resources[resource['path']] = sha(image_path)
                with tempfile.TemporaryDirectory(prefix='issue25-reexport-') as temporary:
                    target = Path(temporary) / 'export'
                    command = [str(binary), '--reexport', str(job / 'document.json'),
                               '--asset-root', str(job), '--out', str(target)]
                    result = subprocess.run(command, cwd=ROOT, env=environment, capture_output=True, text=True)
                    assert result.returncode == 0, result.stderr
                    for name in ('document.json', 'document.md', *resources):
                        assert sha(job / name) == sha(target / name), name
                records[spec['id']] = dict(errors=[], document_sha256=document_sha,
                    resources_sha256=resources, schema_sha256=sha(schema), command=command,
                    reexport_returncode=result.returncode)
        except (OSError, ValueError, AssertionError, jsonschema.ValidationError) as error:
            errors.append(str(error))
            records[spec['id']] = dict(errors=errors, document_sha256=document_sha,
                                       resources_sha256=resources)
        print(spec['id'], 'passed' if not errors else errors, flush=True)
    failures = {key: item['errors'] for key, item in records.items() if item['errors']}
    intact = candidate_intact(candidate_run, candidate)
    save(run / 'real-export-verification.json', dict(records=records, failures=failures,
        pages_checked=len(records), evaluation_sha256=sha(run / 'evaluation.json'),
        candidate_intact=intact, passed=not failures and intact and len(records) == len(manifest['pages'])))
    return int(bool(failures) or not intact)


if __name__ == '__main__':
    raise SystemExit(main())
