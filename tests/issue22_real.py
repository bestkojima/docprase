"""真实 MNN 公共作业：取消、生成超时、安全重建与下一作业。"""
import ctypes as c
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time

from issue22_retry import Jobs
from config_abi import Bytes, View
from printed_page_integration import ROOT


def verify_reexport(binary, source, target):
    process = subprocess.run([str(binary), '--reexport', str(source / 'document.json'),
        '--asset-root', str(source), '--out', str(target)], capture_output=True, text=True)
    assert process.returncode == 0, process.stderr
    for name in ('document.json', 'document.md'):
        assert (target / name).read_bytes() == (source / name).read_bytes()
    for asset in (source / 'assets').iterdir():
        assert (target / 'assets' / asset.name).read_bytes() == asset.read_bytes()


def main():
    os.chdir(ROOT)
    library, binary, destination = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve(), Path(sys.argv[3]).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    setting = json.loads((ROOT / 'configs/printed-page.example.json').read_text())
    setting['execution']['max_new_tokens'] = 8
    jobs = Jobs(library, setting, (ROOT / 'tests/fixtures/ovis/chinese_text.png').read_bytes())
    job = jobs.create()
    outcome, events = [], []
    worker = threading.Thread(target=lambda: outcome.append(jobs.lib.dococr_job_run(job, c.byref(jobs.input))))
    worker.start()
    deadline = time.monotonic() + 60
    while worker.is_alive() and time.monotonic() < deadline:
        buffer = Bytes()
        code = jobs.lib.dococr_job_next_event(job, c.byref(buffer))
        if code == 0:
            event = json.loads(jobs.read(buffer))
            events.append(event)
            if event['kind'] == 'region_started':
                assert jobs.lib.dococr_job_cancel(job) == 0
                break
        else:
            assert code == 8, code
            time.sleep(.005)
    worker.join(timeout=180)
    assert not worker.is_alive() and outcome == [7], outcome
    status = jobs.metadata('job_status', job)
    manifest = jobs.metadata('job_manifest', job)
    assert status['state'] == 'cancelled' and status['engine_ready']
    assert all(len(r.get('recognition', {}).get('attempts', [])) <= 1 for r in manifest['regions'])
    assert jobs.lib.dococr_job_destroy(job) == 0
    (destination / 'cancel-status.json').write_text(json.dumps(status, ensure_ascii=False, indent=2)+'\n')
    (destination / 'cancel-manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+'\n')
    print('real cancellation: safely returned, engine ready', flush=True)
    job = jobs.create()
    assert jobs.lib.dococr_job_run(job, c.byref(jobs.input)) == 0
    doc, markdown = jobs.save(job, destination / 'recovered')
    status = jobs.metadata('job_status', job)
    assert status['engine_ready'] and status['terminal']
    reference = (ROOT / 'tests/fixtures/ovis/chinese_text.reference.txt').read_text().strip()
    assert reference in markdown, markdown
    blocks = doc['pages'][0]['blocks']
    assert any(b['status'] == 'ok' and len(b['provenance']['recognition']['attempts']) == 2 for b in blocks)
    assert jobs.lib.dococr_job_destroy(job) == 0
    jobs.close()
    verify_reexport(binary, destination / 'recovered', destination / 'reexport')
    print('real recovery: token retry completed, reference matched, reexport identical', flush=True)

    setting['execution']['generation_timeout_ms'] = 1
    jobs = Jobs(library, setting, (ROOT / 'tests/fixtures/ovis/chinese_text.png').read_bytes())
    job = jobs.create()
    assert jobs.lib.dococr_job_run(job, c.byref(jobs.input)) == 0
    timed, markdown = jobs.save(job, destination / 'timeout')
    status = jobs.metadata('job_status', job)
    (destination / 'timeout-status.json').write_text(json.dumps(status, ensure_ascii=False, indent=2)+'\n')
    (destination / 'timeout-manifest.json').write_text(json.dumps(jobs.metadata('job_manifest', job), ensure_ascii=False, indent=2)+'\n')
    attempts = [a for b in timed['pages'][0]['blocks'] for a in b['provenance']['recognition']['attempts']]
    assert any(a['output']['stop_reason'] == 'timeout' for a in attempts), attempts
    assert all(len(b['provenance']['recognition']['attempts']) <= 1 for b in timed['pages'][0]['blocks'])
    assert status['engine_ready'] and '![原图]' in markdown
    assert jobs.lib.dococr_job_destroy(job) == 0
    setting['execution']['generation_timeout_ms'] = 120000
    payload = json.dumps(setting).encode()
    assert jobs.lib.dococr_reconfigure(jobs.engine, View(payload, len(payload))) == 0
    job = jobs.create()
    assert jobs.lib.dococr_job_run(job, c.byref(jobs.input)) == 0
    after_timeout, recovered_markdown = jobs.save(job, destination / 'after-timeout')
    assert reference in recovered_markdown
    status = jobs.metadata('job_status', job)
    (destination / 'after-timeout-status.json').write_text(json.dumps(status, ensure_ascii=False, indent=2)+'\n')
    assert status['engine_ready'], status
    assert jobs.lib.dococr_job_destroy(job) == 0
    jobs.close()
    for name in ('timeout', 'after-timeout'):
        verify_reexport(binary, destination / name, destination / ('reexport-' + name))
    (destination / 'summary.json').write_text(json.dumps(dict(cancelled_safely=True,
        recovered_reference=reference, recovered_status=doc['status'],
        timeout_stop_reasons=[a['output']['stop_reason'] for a in attempts],
        timeout_generation_ms=[a['output']['generation_elapsed_ms'] for a in attempts],
        after_timeout_status=after_timeout['status'], after_timeout_reference_matched=True,
        reexport_identical=True), ensure_ascii=False, indent=2)+'\n')
    print('real timeout: no retry, safely rebuilt, next document reference matched', flush=True)


if __name__ == '__main__':
    main()
