"""Copy compact raw evidence and SHA-index generated assets for review."""
import hashlib
import json
from pathlib import Path
import shutil
import sys


ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    run = Path(sys.argv[1]).resolve()
    evidence = ROOT / 'docs/issue-15/evidence'
    evidence.mkdir(parents=True, exist_ok=True)
    for name in ('summary.json', 'quality.json', 'performance.json'):
        shutil.copy2(run / name, evidence / name)
    inventory = {}
    for sample in json.loads((ROOT / 'docs/issue-15/samples.json').read_text())['samples']:
        name = sample['id']
        source = run / name
        target = evidence / name
        target.mkdir(parents=True, exist_ok=True)
        for filename in ('command.json', 'time.txt', 'stdout.log', 'stderr.log'):
            shutil.copy2(source / filename, target / filename)
        job = source / 'job'
        for filename in ('document.json', 'document.md', 'run-manifest.json',
                         'job-status.json', 'job-events.jsonl', 'execution-plan.json'):
            shutil.copy2(job / filename, target / filename)
        assets = sorted(p for p in (job / 'assets').rglob('*') if p.is_file())
        inventory[name] = {'source_sha256': sample['sha256'],
                           'assets': {str(p.relative_to(job)): digest(p) for p in assets},
                           'reexport_json_sha256': digest(source / 'reexport/document.json'),
                           'reexport_markdown_sha256': digest(source / 'reexport/document.md')}
    for path in (ROOT / 'output/issue-15/environment').iterdir():
        if path.is_file():
            shutil.copy2(path, evidence / path.name)
    extra = {
        'reading-fix': ROOT / 'output/issue-15/reading-fix-20260927',
        'abi-continuous': ROOT / 'output/issue-15/abi-continuous',
        'negative': ROOT / 'output/issue-15/negative-final',
    }
    for name, folder in extra.items():
        target = evidence / name
        target.mkdir(parents=True, exist_ok=True)
        if name == 'reading-fix':
            selected = ('document.json', 'document.md', 'run-manifest.json',
                        'job-status.json', 'job-events.jsonl', 'execution-plan.json')
            paths = [folder / filename for filename in selected]
            inventory[name] = {'assets': {str(p.relative_to(folder)): digest(p) for p in
                                          sorted((folder / 'assets').rglob('*')) if p.is_file()},
                               'reexport_json_sha256': digest(ROOT / 'output/issue-15/reading-fix-reexport/document.json'),
                               'reexport_markdown_sha256': digest(ROOT / 'output/issue-15/reading-fix-reexport/document.md')}
        elif name == 'abi-continuous':
            paths = sorted(p for p in folder.iterdir() if p.is_file())
        else:
            paths = [folder / 'summary.json', *sorted(folder.glob('*.stderr.log'))]
        for path in paths:
            shutil.copy2(path, target / path.name)
    for filename in ('reading-fix-quality.json', 'reading-fix-time.txt',
                     'reading-fix-stdout.log', 'reading-fix-stderr.log',
                     'reading-fix-reexport.stdout.log', 'reading-fix-reexport.stderr.log',
                     'order-audit.json', 'quality-final.log',
                     'unittest-dependency-reproduction.log',
                     'negative-final.stdout.log', 'negative-final.stderr.log',
                     'red-test.log', 'green-test.log', 'local-title-test.log',
                     'final-build.log', 'final-ctest.log', 'final-unittest.log'):
        path = ROOT / 'output/issue-15' / filename
        if path.is_file():
            shutil.copy2(path, evidence / filename)
    (evidence / 'asset-sha256.json').write_text(json.dumps(inventory, ensure_ascii=False,
                                                          indent=2) + '\n')
    checksums = []
    for path in sorted(p for p in evidence.rglob('*') if p.is_file() and p.name != 'SHA256SUMS'):
        checksums.append(f'{digest(path)}  {path.relative_to(evidence)}')
    (evidence / 'SHA256SUMS').write_text('\n'.join(checksums) + '\n')
    print(f'archived {len(checksums)} compact files; {sum(len(x["assets"]) for x in inventory.values())} asset hashes')


if __name__ == '__main__':
    main()
