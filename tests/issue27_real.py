"""验证本轮真实全集的 Schema、原始字节、资源及生产重新导出。"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

import jsonschema
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from issue24_run import save, sha
from issue27_evidence import check_frozen, read


def main():
    run = Path(sys.argv[1]).resolve()
    record, _, binary, _, environment = check_frozen(run, Path(__file__))
    if '--safety' in sys.argv[2:]:
        destination = run / 'safety'
        destination.mkdir(exist_ok=False)
        command = [sys.executable, str(ROOT / 'tests/issue22_real.py'),
                   str(binary.parent / 'libdococr_c.so'), str(binary), str(destination)]
        with (destination / 'stdout.log').open('w') as stdout, (destination / 'stderr.log').open('w') as stderr:
            result = subprocess.run(command, cwd=ROOT, env=environment, stdout=stdout, stderr=stderr, timeout=600)
        check_frozen(run, Path(__file__))
        summary = destination / 'summary.json'
        save(run / 'safety-verification.json', dict(returncode=result.returncode,
            candidate_sha256=record['candidate_sha256'], evaluation_sha256=sha(run / 'evaluation.json'),
            summary_sha256=sha(summary) if summary.exists() else None, command=command))
        return result.returncode
    records = {}
    for job_spec in record['jobs']:
        folder = run / job_spec['key']
        job = folder / 'job'
        errors, resources, document_sha = [], {}, None
        try:
            receipt = read(folder / 'command.json')
            for name, digest in receipt['files_sha256'].items():
                path = (folder / name).resolve()
                if not path.is_relative_to(folder.resolve()) or sha(path) != digest:
                    raise ValueError(f'原始产物 SHA 不符：{name}')
            document = read(job / 'document.json')
            document_sha = sha(job / 'document.json')
            kind = document['source']['type']
            version = document['schema_version']
            schema = run / f'freeze/docs/issue-26/document-ir-{version}-{kind}.schema.json'
            jsonschema.validate(document, read(schema))
            for resource in document['resources']:
                source = (job / resource['path']).resolve()
                if not source.is_relative_to(job.resolve()) or not source.is_file():
                    raise ValueError(f"资源缺失或越界：{resource['path']}")
                with Image.open(source) as image:
                    if image.size != (resource['width'], resource['height']):
                        raise ValueError(f"资源尺寸不符：{resource['path']}")
                    image.verify()
                resources[resource['path']] = sha(source)
            with tempfile.TemporaryDirectory(prefix='issue27-reexport-') as temporary:
                target = Path(temporary) / 'export'
                command = [str(binary), '--reexport', str(job / 'document.json'),
                           '--asset-root', str(job), '--out', str(target)]
                result = subprocess.run(command, cwd=ROOT, env=environment, capture_output=True, text=True)
                if result.returncode != 0:
                    raise ValueError(f'生产重新导出失败：{result.stderr}')
                for name in ('document.json', 'document.md', *resources):
                    if sha(job / name) != sha(target / name):
                        raise ValueError(f'首次与重新导出不等价：{name}')
            expected_pages = job_spec['spec'].get('pages', 1)
            if len(document['pages']) != expected_pages:
                raise ValueError('页数不符')
            records[job_spec['key']] = dict(errors=[], document_sha256=document_sha,
                resources_sha256=resources, schema_sha256=sha(schema), pages_checked=len(document['pages']))
        except (OSError, ValueError, KeyError, jsonschema.ValidationError) as error:
            errors.append(str(error))
            records[job_spec['key']] = dict(errors=errors, document_sha256=document_sha,
                                          resources_sha256=resources, pages_checked=0)
        print(job_spec['key'], 'passed' if not errors else errors, flush=True)
    check_frozen(run, Path(__file__))
    pages_checked = sum(v['pages_checked'] for v in records.values())
    passed = len(records) == record['expected_jobs'] and pages_checked == record['expected_pages'] and \
             not any(v['errors'] for v in records.values())
    save(run / 'real-export-verification.json', dict(passed=passed, records=records,
        pages_checked=pages_checked, jobs_checked=len(records), evaluation_sha256=sha(run / 'evaluation.json')))
    return int(not passed)


if __name__ == '__main__':
    raise SystemExit(main())
