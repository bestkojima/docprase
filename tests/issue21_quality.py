"""通过公共 C ABI 作业和 JSON 重新导出验证异常隔离。"""
import ctypes as c
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from cli_integration import png_2x2
from config_abi import View, Bytes
from job_control import Input, Result
from printed_page_integration import ROOT, config


def main():
    os.chdir(ROOT)
    lib = c.CDLL(sys.argv[1])
    lib.dococr_create.argtypes = [View, c.POINTER(c.c_uint64)]
    lib.dococr_job_create.argtypes = [c.c_uint64, c.POINTER(c.c_uint64)]
    lib.dococr_job_run.argtypes = [c.c_uint64, c.POINTER(Input)]
    lib.dococr_job_result.argtypes = [c.c_uint64, c.POINTER(Result)]
    lib.dococr_job_asset_count.argtypes = [c.c_uint64, c.POINTER(c.c_size_t)]
    lib.dococr_job_asset.argtypes = [c.c_uint64, c.c_size_t, c.POINTER(Bytes), c.POINTER(Bytes)]
    lib.dococr_job_destroy.argtypes = [c.c_uint64]
    lib.dococr_destroy.argtypes = [c.c_uint64]
    lib.dococr_bytes_free.argtypes = [c.POINTER(Bytes)]

    def read(buffer):
        value = c.string_at(buffer.data, buffer.size)
        assert lib.dococr_bytes_free(c.byref(buffer)) == 0
        return value

    setting = json.dumps(config('printed_page_quality')).encode()
    engine = c.c_uint64()
    assert lib.dococr_create(View(setting, len(setting)), c.byref(engine)) == 0
    image = png_2x2()
    owned = (c.c_uint8 * len(image)).from_buffer_copy(image)
    request = Input(c.sizeof(Input), owned, len(image), 1, 0, 0, 0, 0, 0, 0, 0, 0)
    with tempfile.TemporaryDirectory(prefix='dococr-quality-') as temporary:
        root = Path(temporary)
        fixture = root / 'output.json'
        os.environ['DOCOCR_TEST_OUTPUT_PATH'] = str(fixture)

        def check(name, output, state, marker, forbidden, block_index=0):
            fixture.write_text(json.dumps(output))
            job = c.c_uint64()
            assert lib.dococr_job_create(engine, c.byref(job)) == 0
            try:
                assert lib.dococr_job_run(job, c.byref(request)) == 0
                result = Result(c.sizeof(Result))
                assert lib.dococr_job_result(job, c.byref(result)) == 0
                source = read(result.json)
                markdown = read(result.markdown)
                doc = json.loads(source)
                block = doc['pages'][0]['blocks'][block_index]
                assert block['provenance']['assessment']['state'] == state, block
                assert block['provenance']['raw_output'] == output.get('raw_output', output['text'])
                assert marker.encode() in markdown
                if forbidden:
                    assert forbidden.encode() not in markdown
                import jsonschema
                schema = json.loads((ROOT / 'schemas/document-ir/document-ir-1.10-image.schema.json').read_text())
                assert doc['schema_version'] == '1.10'
                jsonschema.validate(doc, schema)
                assert doc['pages'][0]['blocks'][3]['status'] == 'ok'
                assert '中文，English!'.encode() in markdown
                saved = root / name
                saved.mkdir()
                (saved / 'document.json').write_bytes(source)
                count = c.c_size_t()
                assert lib.dococr_job_asset_count(job, c.byref(count)) == 0
                for index in range(count.value):
                    name_buffer, buffer = Bytes(), Bytes()
                    assert lib.dococr_job_asset(job, index, c.byref(name_buffer), c.byref(buffer)) == 0
                    path = read(name_buffer).decode()
                    target = saved / path
                    target.parent.mkdir(exist_ok=True)
                    target.write_bytes(read(buffer))
                for resource in doc['resources']:
                    assert (saved / resource['path']).read_bytes().startswith(b'\x89PNG')
                reexport = root / (name + '-reexport')
                process = subprocess.run([sys.argv[2], '--reexport', str(saved / 'document.json'),
                    '--asset-root', str(saved), '--out', str(reexport)], capture_output=True, text=True)
                assert process.returncode == 0, process.stderr
                assert (reexport / 'document.md').read_bytes() == markdown
                assert (reexport / 'document.json').read_bytes() == source
                return doc, saved
            finally:
                assert lib.dococr_job_destroy(job) == 0

        raw = (ROOT / 'docs/issue-21/evidence/caption-numbering.txt').read_text()
        check('numbering', dict(text=raw, finish_reason='truncated', stop_reason='token_limit'),
              'anomalous', '[确认异常：b0001]', '## 1')
        check('truncated', dict(text='TOKEN_CUT_SENTINEL', finish_reason='truncated', stop_reason='token_limit'),
              'incomplete', '[识别不完整：b0001]', 'TOKEN_CUT_SENTINEL')
        check('runtime', dict(text='', raw_output='ERROR_RAW_SENTINEL', finish_reason='failed',
              stop_reason='error', error='controlled_failure'),
              'failed', '[识别失败：b0001]', 'ERROR_RAW_SENTINEL')
        check('unverified', dict(text='suspect phrase repeated!\n' * 3, finish_reason='complete',
              stop_reason='normal'), 'unverified', '[待核验：b0001]', 'suspect phrase')
        normal, normal_saved = check('normal', dict(text='1. 计算面积。\n2. 计算体积。\n3. 说明原因。',
              finish_reason='complete', stop_reason='normal'), 'ok', '1. 计算面积。', None)
        check('missing-finish', dict(text='MISSING_FINISH_SENTINEL', finish_reason='', stop_reason='normal'),
              'unverified', '[待核验：b0001]', 'MISSING_FINISH_SENTINEL')
        long_text = '\n'.join(f'{i}. 请计算第{i}题，说明推导过程和结果。' for i in range(1, 201))
        check('long', dict(text=long_text, finish_reason='complete', stop_reason='normal'),
              'ok', long_text, None)
        check('no-vision', dict(text='SPURIOUS_SENTINEL', finish_reason='complete',
              stop_reason='normal', visual=False), 'failed', '[识别失败：b0001]', 'SPURIOUS_SENTINEL')
        check('transformed-loop', dict(text='CHANGED_OUTPUT_SENTINEL repeated.\n' * 4,
              raw_output='An adapter returned a different value.', finish_reason='complete',
              stop_reason='token_limit'), 'anomalous', '[确认异常：b0001]', 'CHANGED_OUTPUT_SENTINEL')
        check('table-truncated', dict(text='<table><tr><td>TABLE_CUT_SENTINEL', task='table',
              finish_reason='truncated', stop_reason='token_limit'), 'incomplete',
              '[识别不完整：b0003]', 'TABLE_CUT_SENTINEL', block_index=2)
        check('table-empty-text', dict(text='', raw_output='<table><tr><td>RAW_TABLE_SENTINEL</td></tr></table>',
              task='table', finish_reason='complete', stop_reason='normal'), 'failed',
              '[识别失败：b0003]', 'RAW_TABLE_SENTINEL', block_index=2)
        table = '<table>' + '<tr><td>合法相同单元格</td></tr>' * 10 + '</table>'
        check('table-repeated-cells', dict(text=table, task='table', finish_reason='complete',
              stop_reason='normal'), 'ok', '合法相同单元格', None, block_index=2)
        _, saved = check('legacy-seed', dict(text='正常的旧文档。', finish_reason='complete',
              stop_reason='normal'), 'ok', '正常的旧文档。', None)
        legacy = json.loads((saved / 'document.json').read_text())
        legacy.pop('export_policy', None)
        for page in legacy['pages']:
            for layout in page['layout_blocks']:
                layout.pop('semantic_label', None)
            for block in page['blocks']:
                block.pop('block_order', None)
        legacy['schema_version'] = '1.5'
        legacy['pages'][0].pop('structure_plan')
        for region in legacy['pages'][0]['regions']:
            region.pop('recognition_type')
        for block in legacy['pages'][0]['blocks']:
            block['provenance'].pop('assessment')
            block['provenance'].pop('recognition', None)
        block = legacy['pages'][0]['blocks'][0]
        block['content']['text'] = block['provenance']['raw_output'] = raw
        block['provenance']['visual']['stop_reason'] = 'token_limit'
        legacy_path = root / 'legacy-numbering.json'
        legacy_path.write_text(json.dumps(legacy))
        target = root / 'legacy-numbering-reexport'
        process = subprocess.run([sys.argv[2], '--reexport', str(legacy_path),
            '--asset-root', str(saved), '--out', str(target)], capture_output=True, text=True)
        assert process.returncode == 0, process.stderr
        assert '## 1' not in (target / 'document.md').read_text()
        assert '[确认异常：b0001]' in (target / 'document.md').read_text()
        assert (target / 'document.json').read_bytes() == legacy_path.read_bytes()
        assert 'assessment' not in (target / 'document.json').read_text()
        for name in ('evidence', 'content'):
            forged = copy.deepcopy(normal)
            if name == 'evidence':
                forged['pages'][0]['blocks'][0]['provenance']['assessment']['nonempty_lines'] = 0
            else:
                forged['pages'][0]['blocks'][0]['content']['text'] = 'SPOOFED_REPEAT_SENTINEL repeated.\n' * 8
            path = root / f'forged-{name}.json'
            path.write_text(json.dumps(forged))
            target = root / f'forged-{name}-reexport'
            process = subprocess.run([sys.argv[2], '--reexport', str(path),
                '--asset-root', str(normal_saved), '--out', str(target)], capture_output=True, text=True)
            assert process.returncode == 3 and not target.exists(), process.stderr
    assert lib.dococr_destroy(engine) == 0


if __name__ == '__main__':
    main()
