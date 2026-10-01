"""通过公共作业与重新导出验证有界重试和连续作业隔离。"""
import ctypes as c
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time

from cli_integration import png_2x2
from config_abi import View, Bytes
from job_control import Input, Result
from printed_page_integration import ROOT, config


class Jobs:
    def __init__(self, library, setting, image):
        self.lib = c.CDLL(str(library))
        for name, arguments in {
            'create': [View, c.POINTER(c.c_uint64)],
            'reconfigure': [c.c_uint64, View],
            'job_create': [c.c_uint64, c.POINTER(c.c_uint64)],
            'job_run': [c.c_uint64, c.POINTER(Input)],
            'job_result': [c.c_uint64, c.POINTER(Result)],
            'job_manifest': [c.c_uint64, c.POINTER(Bytes)],
            'job_status': [c.c_uint64, c.POINTER(Bytes)],
            'job_next_event': [c.c_uint64, c.POINTER(Bytes)],
            'job_cancel': [c.c_uint64], 'job_destroy': [c.c_uint64],
            'destroy': [c.c_uint64], 'bytes_free': [c.POINTER(Bytes)],
            'job_asset_count': [c.c_uint64, c.POINTER(c.c_size_t)],
            'job_asset': [c.c_uint64, c.c_size_t, c.POINTER(Bytes), c.POINTER(Bytes)],
        }.items():
            getattr(self.lib, 'dococr_' + name).argtypes = arguments
        self.engine = c.c_uint64()
        payload = json.dumps(setting).encode()
        assert self.lib.dococr_create(View(payload, len(payload)), c.byref(self.engine)) == 0
        self.owned = (c.c_uint8 * len(image)).from_buffer_copy(image)
        self.input = Input(c.sizeof(Input), self.owned, len(image), 1, 0, 0, 0, 0, 0, 0, 0, 0)

    def read(self, buffer):
        raw = c.string_at(buffer.data, buffer.size)
        assert self.lib.dococr_bytes_free(c.byref(buffer)) == 0
        return raw

    def metadata(self, name, job):
        buffer = Bytes()
        assert getattr(self.lib, 'dococr_' + name)(job, c.byref(buffer)) == 0
        return json.loads(self.read(buffer))

    def create(self):
        job = c.c_uint64()
        assert self.lib.dococr_job_create(self.engine, c.byref(job)) == 0
        return job

    def save(self, job, target):
        result = Result(c.sizeof(Result))
        assert self.lib.dococr_job_result(job, c.byref(result)) == 0
        source, markdown = self.read(result.json), self.read(result.markdown)
        target.mkdir()
        (target / 'document.json').write_bytes(source)
        (target / 'document.md').write_bytes(markdown)
        count = c.c_size_t()
        assert self.lib.dococr_job_asset_count(job, c.byref(count)) == 0
        for index in range(count.value):
            name, buffer = Bytes(), Bytes()
            assert self.lib.dococr_job_asset(job, index, c.byref(name), c.byref(buffer)) == 0
            path = target / self.read(name).decode()
            path.parent.mkdir(exist_ok=True)
            path.write_bytes(self.read(buffer))
        return json.loads(source), markdown.decode()

    def close(self):
        assert self.lib.dococr_destroy(self.engine) == 0


def main():
    os.chdir(ROOT)
    setting = config('printed_page_quality')
    setting['execution']['max_new_tokens'] = 64
    jobs = Jobs(sys.argv[1], setting, png_2x2())
    for name, invalid in (('generation_timeout_ms', 120001), ('generation_timeout_ms', 0),
                          ('max_new_tokens', 4097)):
        rejected = copy.deepcopy(setting)
        rejected['execution'][name] = invalid
        payload = json.dumps(rejected).encode()
        engine = c.c_uint64()
        assert jobs.lib.dococr_create(View(payload, len(payload)), c.byref(engine)) == 9
        assert engine.value == 0
    with tempfile.TemporaryDirectory(prefix='dococr-retry-') as temporary:
        root = Path(temporary)
        fixture = root / 'output.json'
        os.environ['DOCOCR_TEST_OUTPUT_PATH'] = str(fixture)
        fixture.write_text(json.dumps(dict(text='FIRST_CUT_SENTINEL', finish_reason='truncated',
            stop_reason='token_limit', initial_token_budget=64,
            retry_output=dict(text='第二次完整正文。', finish_reason='complete', stop_reason='normal'))))
        job = jobs.create()
        try:
            assert jobs.lib.dococr_job_run(job, c.byref(jobs.input)) == 0
            doc, markdown = jobs.save(job, root / 'success')
            block = doc['pages'][0]['blocks'][0]
            recognition = block['provenance']['recognition']
            assert len(recognition['attempts']) == 2
            assert recognition['selected_attempt'] == 2
            assert [a['config']['max_new_tokens'] for a in recognition['attempts']] == [64, 4096]
            assert recognition['attempts'][1]['correction'] == 'increase_token_budget'
            assert block['status'] == 'ok' and '第二次完整正文。' in markdown
            assert 'FIRST_CUT_SENTINEL' not in markdown
            assert recognition['attempts'][0]['output']['raw_output'] == 'FIRST_CUT_SENTINEL'
            process = subprocess.run([sys.argv[2], '--reexport', str(root / 'success/document.json'),
                '--asset-root', str(root / 'success'), '--out', str(root / 'reexport')],
                capture_output=True, text=True)
            assert process.returncode == 0, process.stderr
            assert (root / 'reexport/document.md').read_text() == markdown
        finally:
            assert jobs.lib.dococr_job_destroy(job) == 0
        fixture.write_text(json.dumps(dict(text='', finish_reason='failed', stop_reason='vision_missing',
            error='ovis_visual_tokens_missing', visual=False,
            retry_output=dict(text='修正视觉输入后的正文。', finish_reason='complete', stop_reason='normal'))))
        job = jobs.create()
        try:
            assert jobs.lib.dococr_job_run(job, c.byref(jobs.input)) == 0
            doc, markdown = jobs.save(job, root / 'visual-success')
            block = doc['pages'][0]['blocks'][0]
            attempts = block['provenance']['recognition']['attempts']
            assert len(attempts) == 2 and attempts[1]['correction'] == 'increase_visual_resolution'
            assert attempts[1]['config']['max_new_tokens'] == 64
            assert attempts[1]['visual']['scale'] > attempts[0]['visual']['scale']
            assert block['status'] == 'ok' and '修正视觉输入后的正文。' in markdown
            process = subprocess.run([sys.argv[2], '--reexport', str(root / 'visual-success/document.json'),
                '--asset-root', str(root / 'visual-success'), '--out', str(root / 'visual-reexport')],
                capture_output=True, text=True)
            assert process.returncode == 0, process.stderr
        finally:
            assert jobs.lib.dococr_job_destroy(job) == 0
        cut = dict(text='FIRST_CUT_SENTINEL', finish_reason='truncated', stop_reason='token_limit', initial_token_budget=64)

        def check(name, output, state, count, current_jobs=jobs, normal_sibling=True):
            fixture.write_text(json.dumps(output))
            job = current_jobs.create()
            try:
                assert current_jobs.lib.dococr_job_run(job, c.byref(current_jobs.input)) == 0
                doc, markdown = current_jobs.save(job, root / name)
                import jsonschema
                jsonschema.validate(doc, json.loads((ROOT / 'docs/issue-22/document-ir-1.7-image.schema.json').read_text()))
                block = doc['pages'][0]['blocks'][0]
                record = block['provenance']['recognition']
                assert block['provenance']['assessment']['state'] == state, block
                assert len(record['attempts']) == count and record['selected_attempt'] == count
                assert record['configured_generation_budget_ms'] == count * 120000
                assert all(0 < a['config']['max_new_tokens'] <= 4096 for a in record['attempts'])
                if state != 'ok':
                    assert '![原图]' in markdown and 'SENTINEL' not in markdown
                if normal_sibling:
                    assert '中文，English!' in markdown
                process = subprocess.run([sys.argv[2], '--reexport', str(root / name / 'document.json'),
                    '--asset-root', str(root / name), '--out', str(root / (name + '-reexport'))],
                    capture_output=True, text=True)
                assert process.returncode == 0, process.stderr
                assert (root / (name + '-reexport') / 'document.json').read_bytes() == (root / name / 'document.json').read_bytes()
                assert (root / (name + '-reexport') / 'document.md').read_text() == markdown
                return doc
            finally:
                assert current_jobs.lib.dococr_job_destroy(job) == 0

        failed = check('second-cut', dict(cut, retry_output=dict(text='SECOND_CUT_SENTINEL',
            finish_reason='truncated', stop_reason='token_limit')), 'incomplete', 2)
        check('second-failure', dict(cut, retry_output=dict(text='', raw_output='SECOND_ERROR_SENTINEL',
            finish_reason='failed', stop_reason='error', error='controlled_failure')), 'failed', 2)
        check('first-exception', {}, 'failed', 1, normal_sibling=False)
        check('visual-exception', dict(text='', finish_reason='failed', stop_reason='vision_missing',
            error='ovis_visual_tokens_missing', visual=False, retry_output={}), 'failed', 2, normal_sibling=False)
        encoded = check('encoded-first-cut', dict(cut, invalid_utf8=True, retry_output=dict(text='有效完整文字。',
            finish_reason='complete', stop_reason='normal')), 'ok', 2)
        assert encoded['pages'][0]['blocks'][0]['provenance']['recognition']['attempts'][0]['output']['raw_output_base64'] == 'Zmlyc3QgY3V0/w=='
        check('second-timeout', dict(cut, retry_output=dict(text='', raw_output='TIMEOUT_SENTINEL',
            finish_reason='failed', stop_reason='timeout', error='controlled_timeout')), 'failed', 2)
        check('first-timeout', dict(text='', raw_output='TIMEOUT_SENTINEL',
            finish_reason='failed', stop_reason='timeout', error='controlled_timeout'), 'failed', 1)
        check('repetition', dict(text='REPEAT_SENTINEL repeated fragment.\n' * 8,
            finish_reason='truncated', stop_reason='token_limit'), 'anomalous', 1)
        check('unverified', dict(text='REPEAT_SENTINEL repeated fragment.\n' * 8,
            finish_reason='complete', stop_reason='normal'), 'unverified', 1)
        check('visual-uncorrected', dict(text='VISION_SENTINEL', visual=False,
            finish_reason='complete', stop_reason='normal'), 'failed', 1)
        capped_setting = copy.deepcopy(setting)
        capped_setting['execution']['max_new_tokens'] = 4096
        capped = Jobs(sys.argv[1], capped_setting, png_2x2())
        check('capped-cut', cut, 'incomplete', 1, capped)
        capped.close()
        for name in ('third-attempt', 'uncorrected', 'selection', 'budget', 'selected-output'):
            forged = copy.deepcopy(failed)
            record = forged['pages'][0]['blocks'][0]['provenance']['recognition']
            if name == 'third-attempt':
                record['attempts'].append(copy.deepcopy(record['attempts'][1]))
            elif name == 'uncorrected':
                record['attempts'][1]['config']['max_new_tokens'] = 64
            elif name == 'selection':
                record['selected_attempt'] = 1
            elif name == 'budget':
                record['attempts'][1]['config']['generation_timeout_ms'] = 120001
            else:
                record['attempts'][1]['output']['raw_output'] = 'FORGED_SENTINEL'
            path = root / (name + '.json')
            path.write_text(json.dumps(forged))
            target = root / (name + '-reexport')
            process = subprocess.run([sys.argv[2], '--reexport', str(path),
                '--asset-root', str(root / 'second-cut'), '--out', str(target)], capture_output=True, text=True)
            assert process.returncode == 3 and not target.exists(), process.stderr

        gate, entered = root / 'gate', root / 'entered'
        gate.touch()
        fixture.write_text(json.dumps(dict(cut, retry_output=dict(text='CANCEL_SENTINEL',
            finish_reason='complete', stop_reason='normal', gate=str(gate), entered=str(entered)))))
        job = jobs.create()
        outcome = []
        worker = threading.Thread(target=lambda: outcome.append(jobs.lib.dococr_job_run(job, c.byref(jobs.input))))
        worker.start()
        try:
            deadline = time.monotonic() + 10
            while not entered.exists() and worker.is_alive() and time.monotonic() < deadline:
                time.sleep(.005)
            assert entered.exists(), outcome
            assert jobs.lib.dococr_job_cancel(job) == 0
        finally:
            gate.unlink(missing_ok=True)
            worker.join(timeout=10)
        assert not worker.is_alive() and outcome == [7], outcome
        status = jobs.metadata('job_status', job)
        assert status['state'] == 'cancelled' and status['engine_ready']
        manifest = jobs.metadata('job_manifest', job)
        attempts = manifest['regions'][0]['recognition']['attempts']
        assert len(attempts) == 2 and attempts[-1]['output']['stop_reason'] == 'cancelled'
        assert attempts[0]['output']['raw_output'] == 'FIRST_CUT_SENTINEL'
        assert jobs.lib.dococr_job_destroy(job) == 0
        recovered = check('next-job', dict(text='下一作业独立正文。', finish_reason='complete',
            stop_reason='normal'), 'ok', 1)
        assert 'FIRST_CUT_SENTINEL' not in json.dumps(recovered) and 'cancelled retry raw' not in json.dumps(recovered)
    jobs.close()


if __name__ == '__main__':
    main()
