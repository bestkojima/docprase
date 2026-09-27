"""真实 MNN 公共作业在版面开始后取消，重建后同引擎重新解析。"""
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import sys
import threading
import time

from config_abi import View, Bytes
from job_control import Input, Result

ROOT = Path(__file__).resolve().parents[1]


def main():
    library, destination, baseline_path = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    os.chdir(ROOT)
    destination.mkdir(parents=True, exist_ok=True)
    baseline = json.loads(baseline_path.read_text())
    expected_raw_hash = baseline['requests'][0]['matched_raw_sha256']
    lib = c.CDLL(str(library))
    lib.dococr_create.argtypes = [View, c.POINTER(c.c_uint64)]
    lib.dococr_job_create.argtypes = [c.c_uint64, c.POINTER(c.c_uint64)]
    lib.dococr_job_run.argtypes = [c.c_uint64, c.POINTER(Input)]
    lib.dococr_job_cancel.argtypes = [c.c_uint64]
    lib.dococr_job_next_event.argtypes = [c.c_uint64, c.POINTER(Bytes)]
    lib.dococr_job_status.argtypes = [c.c_uint64, c.POINTER(Bytes)]
    lib.dococr_job_manifest.argtypes = [c.c_uint64, c.POINTER(Bytes)]
    lib.dococr_job_result.argtypes = [c.c_uint64, c.POINTER(Result)]
    lib.dococr_job_destroy.argtypes = [c.c_uint64]
    lib.dococr_destroy.argtypes = [c.c_uint64]
    lib.dococr_bytes_free.argtypes = [c.POINTER(Bytes)]

    def read(value):
        raw = c.string_at(value.data, value.size)
        assert lib.dococr_bytes_free(c.byref(value)) == 0
        return raw

    def read_json(function, job):
        value = Bytes()
        assert function(job, c.byref(value)) == 0
        return json.loads(read(value))

    setting = (ROOT / 'configs/printed-page.example.json').read_bytes()
    image = (ROOT / 'tests/fixtures/ovis/source_page.jpg').read_bytes()
    owned = (c.c_uint8 * len(image)).from_buffer_copy(image)
    engine = c.c_uint64()
    assert lib.dococr_create(View(setting, len(setting)), c.byref(engine)) == 0
    job = c.c_uint64()
    assert lib.dococr_job_create(engine, c.byref(job)) == 0
    request = Input(c.sizeof(Input), owned, len(image), 2, 0, 0, 0, 0, 0, 0, 0, 0)
    outcome = []
    worker = threading.Thread(target=lambda: outcome.append(lib.dococr_job_run(job, c.byref(request))))
    worker.start()
    events = []
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        value = Bytes()
        code = lib.dococr_job_next_event(job, c.byref(value))
        if code == 0:
            event = json.loads(read(value))
            events.append(event)
            if event['kind'] == 'layout_started':
                break
        elif code == 8:
            time.sleep(0.01)
        else:
            raise AssertionError(code)
    assert events[-1]['kind'] == 'layout_started', events
    assert lib.dococr_job_cancel(job) == 0
    worker.join(timeout=180)
    assert not worker.is_alive() and outcome == [7], outcome
    while True:
        value = Bytes()
        code = lib.dococr_job_next_event(job, c.byref(value))
        if code == 8:
            break
        assert code == 0
        events.append(json.loads(read(value)))
    assert any(event['kind'] == 'recovery_completed' for event in events)
    assert not any(event['kind'] == 'page_completed' for event in events)
    status = read_json(lib.dococr_job_status, job)
    assert status['state'] == 'cancelled' and status['terminal'] and status['engine_ready']
    assert status['page_completed'] == 0
    manifest = read_json(lib.dococr_job_manifest, job)
    assert manifest['job_status'] == 'cancelled'
    assert lib.dococr_job_destroy(job) == 0

    retry = c.c_uint64()
    assert lib.dococr_job_create(engine, c.byref(retry)) == 0
    started = time.monotonic()
    assert lib.dococr_job_run(retry, c.byref(request)) == 0
    elapsed = round(time.monotonic() - started, 3)
    result = Result(c.sizeof(Result))
    assert lib.dococr_job_result(retry, c.byref(result)) == 0
    document = json.loads(read(result.json))
    read(result.markdown)
    reference = (ROOT / 'tests/fixtures/ovis/chinese_text.reference.txt').read_text().strip()
    matches = [block for block in document['pages'][0]['blocks']
               if block['type'] == 'text' and block['status'] == 'ok'
               and block['content']['text'].strip() == reference]
    assert matches
    raw_hash = hashlib.sha256(matches[0]['provenance']['raw_output'].encode()).hexdigest()
    assert raw_hash == expected_raw_hash
    retry_status = read_json(lib.dococr_job_status, retry)
    assert retry_status['terminal'] and retry_status['engine_ready']
    assert lib.dococr_job_destroy(retry) == 0
    assert lib.dococr_destroy(engine) == 0
    report = {'cancel_status': outcome[0], 'cancel_state': status['state'],
              'cancel_terminal': status['terminal'], 'recovery_completed': True,
              'same_public_engine': True, 'next_job_status': retry_status['state'],
              'next_elapsed_seconds': elapsed, 'matched_raw_sha256': raw_hash,
              'baseline_matched_raw_sha256': expected_raw_hash,
              'cancel_events': [item['kind'] for item in events]}
    (destination / 'cancel-and-next.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(report, ensure_ascii=False))


if __name__ == '__main__':
    main()
