"""Read-only audit of retained Issue #26 production evidence, plus new reexports."""
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path('/home/dr/.codex/worktrees/0fbc/docprase')
OLD = Path('/home/dr/project/docprase/output/issue-26/acceptance-20-v1.9')
DATA = Path('/home/dr/project/docprase/output/omnidocbench/selected-20')
OUT = Path('/home/dr/project/docprase/output/issue-26/closure-evidence-0fbc-pass')
CLI = Path('/tmp/docprase-issue26-reverify-0fbc/dococr_cli')
sys.path.insert(0, str(ROOT / 'scripts'))
from issue26_annotations import block_candidates, check_options

def read(p):
    return json.loads(p.read_text())

def sha(p):
    with p.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()

def require(ok, message):
    if not ok:
        raise ValueError(message)

OUT.mkdir(parents=True, exist_ok=False)
freeze = read(OLD / 'freeze.json')
report_path = ROOT / 'docs/issue-26/evidence/acceptance-20-v1.9.json'
report = read(report_path)
binding = read(ROOT / 'docs/issue-26/evidence/acceptance-20-v1.9-submission.json')
require(sha(OLD / 'freeze.json') == binding['freeze_sha256'] == report['candidate_freeze_sha256'], 'freeze identity')
require(sha(report_path) == binding['acceptance_sha256'], 'retained acceptance report identity')
for name, digest in freeze['source_sha256'].items():
    require(sha(ROOT / name) == digest, 'submitted source changed: ' + name)
    committed = subprocess.check_output(['git', 'show', binding['implementation_commit'] + ':' + name], cwd=ROOT)
    require(hashlib.sha256(committed).hexdigest() == digest, 'implementation commit source differs: ' + name)
for name, digest in freeze['model_files'].items():
    require(sha(OLD / name) == digest, 'model changed: ' + name)
for name, record in freeze['runtime'].items():
    require(sha(OLD / '.runtime' / name) == record['sha256'], 'runtime changed: ' + name)
require(sha(OLD / '.runtime/config.json') == freeze['config_sha256'], 'config changed')
require(sha(DATA / 'OmniDocBench.json') == freeze['annotation_sha256'], 'annotation changed')
require(report['complete_twenty_page_acceptance'] and not report['failures'], 'full acceptance incomplete')
manifest_path = ROOT / 'docs/omnidocbench-20/manifest.json'
require(sha(manifest_path) == freeze['manifest_sha256'], 'data manifest changed')
specs = {p['id']: p for p in read(manifest_path)['pages']}
env = dict(os.environ, LD_LIBRARY_PATH='/tmp/docprase-issue26-reverify-0fbc:/home/dr/project/MNN/build:/home/dr/project/MNN/build/tools/cv:/home/dr/project/MNN/build/tools/audio:/home/dr/project/MNN/build/express')
rows = []
for row in report['pages']:
    pid = row['page_id']
    job = OLD / pid / 'job'
    require(sha(DATA / specs[pid]['image_path']) == row['source_image_sha256'], pid + ' original image changed')
    require(sha(OLD / pid / 'command.json') == row['command_receipt_sha256'], pid + ' command changed')
    for name, digest in row['artifact_sha256'].items():
        require(sha(job / name) == digest, pid + ' artifact changed: ' + name)
    doc = read(job / 'document.json')
    page = doc['pages'][0]
    plan = page['structure_plan']
    require(plan['block_order'] == page['reading_order'], pid + ' structure/order mismatch')
    require(not row['common_GT_order']['newly_wrong'], pid + ' newly wrong GT order')
    for k in ['structure_checks_passed', 'ownership_unique', 'candidate_evidence_and_region_crops_match_captured_baseline', 'schedule_equals_pre_recognition_plan', 'first_export_equals_reexport']:
        require(row[k], pid + ' failed retained condition: ' + k)
    command = [str(CLI), '--reexport', str(job / 'document.json'), '--asset-root', str(job), '--out', str(OUT / pid)]
    result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True)
    (OUT / (pid + '-reexport.log')).write_text(result.stdout + result.stderr)
    require(result.returncode == 0, pid + ' new production reexport failed: ' + result.stderr)
    require((job / 'document.md').read_bytes() == (OUT / pid / 'document.md').read_bytes(), pid + ' Markdown changed')
    require(read(OUT / pid / 'document.json') == doc, pid + ' IR changed')
    require(all(sha(OUT / pid / r['path']) == sha(job / r['path']) for r in doc['resources']), pid + ' crop resource changed')
    if pid == 'odb-03':
        require(check_options(doc), 'Q17/18 order')
        ids = block_candidates(page)
        blocks = {b['id']: b for b in page['blocks']}
        expected = [([8, 5, 6, 13], [32, 27, 36, 33]), ([4, 1, 0, 2], [26, 31, 25, 30])]
        require(len(plan['groups']) == 2 and len(plan['captions']) == 8, 'Q17/18 incomplete groups')
        for group, (images, captions) in zip(plan['groups'], expected):
            require([ids[i['image_block_id']] for i in group['items']] == [[c] for c in images], 'Q17/18 image binding')
            require([ids[i['caption_block_id']] for i in group['items']] == [[c] for c in captions], 'Q17/18 label binding')
            # The actual saved OCR labels are Markdown headings; preserve them verbatim.
            require([blocks[i['caption_block_id']]['content']['text'] for i in group['items']] == ['## ' + c + '\n' for c in 'ABCD'], 'Q17/18 real OCR label text')
    rows.append({'page_id': pid, 'hashed_artifacts': len(row['artifact_sha256']), 'reexport_command': command, 'reexport_returncode': result.returncode, 'markdown_json_resources_equal': True})
    print(pid + ': retained hashes and freshly built production reexport PASS', flush=True)
png_index = read(ROOT / 'docs/issue-26/acceptance-1.9-png/index.json')
for item in png_index['files']:
    require(sha(ROOT / 'docs/issue-26' / item['path']) == item['sha256'] == sha(Path(item['source'])), 'PNG changed: ' + item['path'])
png_manifest = read(ROOT / 'docs/issue-26/evidence/acceptance-20-v1.9-png-manifest.json')
for item in png_manifest['pages']:
    require(sha(Path(item['source_job']) / 'document.json') == item['source_document_sha256'], 'PNG source document changed')
summary = {'checked_at_utc': datetime.now(timezone.utc).isoformat(), 'implementation_commit': binding['implementation_commit'], 'reviewed_head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(), 'scope': 'retained_twenty_page_hash_audit_and_new_production_reexports_not_new_twenty_page_inference', 'old_production_run': str(OLD), 'source_files_matched': len(freeze['source_sha256']), 'source_commit_binding_verified': True, 'model_runtime_data_hashes_verified': True, 'hashed_artifacts': sum(r['hashed_artifacts'] for r in rows), 'submitted_pngs_verified': len(png_index['files']), 'png_source_documents_verified': len(png_manifest['pages']), 'rebuilt_cli_sha256': sha(CLI), 'rebuilt_library_sha256': sha(CLI.parent / 'libdococr_c.so'), 'q17_q18_order_and_eight_real_labels_passed': True, 'pages': rows, 'passed': len(rows) == 20}
(OUT / 'summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n')
print('PASS: 20 retained pages, all sources/models/runtime/artifacts, Q17/18 eight real labels, and new production reexports', flush=True)
