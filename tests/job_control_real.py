"""真实双模型通过同一公共引擎连续解析两次固定教材页。"""
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import sys
import time

from config_abi import View, Bytes
from job_control import Input, Result

ROOT = Path(__file__).resolve().parents[1]


def main():
    library, destination = Path(sys.argv[1]), Path(sys.argv[2])
    os.chdir(ROOT)
    destination.mkdir(parents=True, exist_ok=True)
    lib = c.CDLL(str(library))
    lib.dococr_create.argtypes = [View, c.POINTER(c.c_uint64)]
    lib.dococr_job_create.argtypes = [c.c_uint64, c.POINTER(c.c_uint64)]
    lib.dococr_job_run.argtypes = [c.c_uint64, c.POINTER(Input)]
    lib.dococr_job_result.argtypes = [c.c_uint64, c.POINTER(Result)]
    lib.dococr_job_manifest.argtypes = [c.c_uint64, c.POINTER(Bytes)]
    lib.dococr_job_status.argtypes = [c.c_uint64, c.POINTER(Bytes)]
    lib.dococr_job_destroy.argtypes = [c.c_uint64]
    lib.dococr_destroy.argtypes = [c.c_uint64]
    lib.dococr_bytes_free.argtypes = [c.POINTER(Bytes)]

    def read(buffer):
        value = c.string_at(buffer.data, buffer.size)
        assert lib.dococr_bytes_free(c.byref(buffer)) == 0
        return value

    source = ROOT / 'tests/fixtures/ovis/source_page.jpg'
    image = source.read_bytes()
    assert hashlib.sha256(image).hexdigest() == 'c8cf71eb2f717727dc2d8a3ae5da1e388f6be7bb1e2c4addbde5d40dafb270f6'
    owned = (c.c_uint8 * len(image)).from_buffer_copy(image)
    setting = (ROOT / 'configs/printed-page.example.json').read_bytes()
    engine = c.c_uint64()
    assert lib.dococr_create(View(setting, len(setting)), c.byref(engine)) == 0
    reference = (ROOT / 'tests/fixtures/ovis/chinese_text.reference.txt').read_text().strip()
    reports = []
    raw_values = []
    for index in range(2):
        job = c.c_uint64()
        assert lib.dococr_job_create(engine, c.byref(job)) == 0
        request = Input(c.sizeof(Input), owned, len(image), 2, 0, 0, 0, 0, 0, 0, 0, 0)
        started = time.monotonic()
        run_status = lib.dococr_job_run(job, c.byref(request))
        elapsed = round(time.monotonic() - started, 3)
        assert run_status == 0, run_status
        result = Result(c.sizeof(Result))
        assert lib.dococr_job_result(job, c.byref(result)) == 0
        document_bytes = read(result.json)
        read(result.markdown)
        (destination / f'job-{index}-document.json').write_bytes(document_bytes)
        document = json.loads(document_bytes)
        matches = [block for block in document['pages'][0]['blocks']
                   if block['type'] == 'text' and block['status'] == 'ok'
                   and block['content']['text'].strip() == reference]
        assert matches, '真实教材正文未匹配固定真值'
        raw_values.append(matches[0]['provenance']['raw_output'])
        (destination / f'job-{index}-matched-raw.txt').write_text(raw_values[-1])
        manifest_bytes = Bytes()
        assert lib.dococr_job_manifest(job, c.byref(manifest_bytes)) == 0
        manifest_raw = read(manifest_bytes)
        (destination / f'job-{index}-manifest.json').write_bytes(manifest_raw)
        manifest = json.loads(manifest_raw)
        status_bytes = Bytes()
        assert lib.dococr_job_status(job, c.byref(status_bytes)) == 0
        status_raw = read(status_bytes)
        (destination / f'job-{index}-status.json').write_bytes(status_raw)
        status = json.loads(status_raw)
        assert status['terminal'] and status['engine_ready']
        assert manifest['actual_device'] == 'cpu' and len(manifest['regions']) > 0
        reports.append({'index': index, 'run_status': run_status,
                        'document_status': document['status'], 'job_state': status['state'],
                        'regions': len(manifest['regions']), 'elapsed_seconds': elapsed,
                        'matched_block': matches[0]['id'],
                        'matched_raw_sha256': hashlib.sha256(raw_values[-1].encode()).hexdigest(),
                        'document_sha256': hashlib.sha256(document_bytes).hexdigest(),
                        'engine_ready': status['engine_ready']})
        assert lib.dococr_job_destroy(job) == 0
    report = {'source_sha256': hashlib.sha256(image).hexdigest(),
              'same_public_engine': True, 'requests': reports,
              'matched_raw_equal': raw_values[0] == raw_values[1],
              'raw_matches_reference_after_strip': [value.strip() == reference for value in raw_values],
              'raw_lengths': [len(value) for value in raw_values],
              'reference_length': len(reference)}
    (destination / 'summary.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    assert raw_values[0] == raw_values[1], report
    assert raw_values[0].strip() == reference, report
    assert lib.dococr_destroy(engine) == 0
    print(json.dumps(report, ensure_ascii=False))


if __name__ == '__main__':
    main()
