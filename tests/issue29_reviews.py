"""公共 CLI：只有绑定原页/候选证据的确认误检才允许跳过。"""
import copy
import hashlib
import json
import os
import subprocess
from pathlib import Path
import struct
import sys
import tempfile

from PIL import Image
from printed_page_integration import config
from label_pipeline import execute, reexport


def page_hash(image):
    rgb = Image.open(image).convert('RGB')
    return hashlib.sha256(f'{rgb.width}x{rgb.height}:'.encode() + rgb.tobytes()).hexdigest()


def review(image, row, cid=0, decision='confirmed_watermark'):
    # fixture backend 默认 mask 全零；原始模型行 + 完整对应 mask 行。
    prefix = (json.dumps(row[2:6], separators=(',', ':')) + ':').encode()
    digest = hashlib.sha256(prefix + struct.pack('<7f', *row) + bytes(200*200*4)).hexdigest()
    return dict(page_rgb_sha256=page_hash(image), candidate_id=cid,
                candidate_sha256=digest, decision=decision,
                reason='已目视确认重复背景标志，裁图不含题目或插图。')


def main():
    os.chdir(Path(__file__).resolve().parents[1])
    cli, production = sys.argv[1:3]
    with tempfile.TemporaryDirectory(prefix='dococr-issue29-') as tmp:
        root = Path(tmp)
        image = root/'page.png'
        Image.new('RGB', (800, 1200), 'white').save(image)
        row = [14, .95, 30, 200, 90, 240, 0]
        trace = dict(candidates=[row, [14, .95, 200, 200, 350, 350, 1]], outputs=[])
        setting = config('printed_page_structure')
        setting['execution'].update(layout_preprocess='reference', layout_score_threshold=.3)
        before, old, _ = execute(cli, root/'before', image, setting, trace)
        setting['execution']['layout_candidate_reviews'] = [review(image, row)]
        after, doc, md = execute(cli, root/'after', image, setting, trace)
        assert doc['schema_version'] == '1.12'
        assert [l['candidate_id'] for l in doc['pages'][0]['layout_blocks']] == [1]
        diag = doc['layout_diagnostics']
        assert diag['candidates'][0]['filter_reason'] == 'review_confirmed_watermark'
        evidence = diag['candidate_reviews']
        assert evidence['decisions'][0]['outcome'] == 'skipped'
        assert (after/evidence['source_asset']).is_file()
        assert (after/evidence['decisions'][0]['crop_asset']).is_file()
        assert (after/diag['candidates'][0]['mask_asset']).is_file()
        assert md.count('![插图]') == 1
        assert json.loads((after/'run-manifest.json').read_text())['regions'] == []
        reexport(production, after, root/'reexport')
        for asset in [evidence['source_asset'], evidence['decisions'][0]['crop_asset'], diag['candidates'][0]['mask_asset']]:
            assert (root/'reexport'/asset).read_bytes() == (after/asset).read_bytes()
        # 模糊判断、原页变化、模型候选变化都不允许静默删除。
        for name, mutate, outcome in [
            ('suspected', lambda r: r.update(decision='suspected'), 'retained'),
            ('keep', lambda r: r.update(decision='keep'), 'retained'),
            ('other-page', lambda r: r.update(page_rgb_sha256='0'*64), 'page_mismatch'),
            ('other-row', lambda r: r.update(candidate_sha256='0'*64), 'candidate_mismatch'),
            ('missing-row', lambda r: r.update(candidate_id=299), 'candidate_mismatch'),
        ]:
            trial = copy.deepcopy(setting)
            mutate(trial['execution']['layout_candidate_reviews'][0])
            job, retained, _ = execute(cli, root/name, image, trial, trace)
            assert retained['pages'] == old['pages'], name
            assert retained['layout_diagnostics']['candidate_reviews']['decisions'][0]['outcome'] == outcome
            reexport(production, job, root/(name+'-reexport'))
        # 同一原页/原始张量在不同前处理映射下产生不同裁图，旧复核也必须失效。
        trial = copy.deepcopy(setting)
        trial['execution'].update(layout_preprocess='smartresize_lanczos')
        trial['execution'].pop('layout_candidate_reviews')
        _, mapped_before, _ = execute(cli, root/'mapped-before', image, trial, trace)
        trial['execution']['layout_candidate_reviews'] = [review(image, trace['candidates'][1], 1)]
        _, mapped_after, _ = execute(cli, root/'mapped-after', image, trial, trace)
        assert mapped_after['pages'] == mapped_before['pages']
        assert mapped_after['layout_diagnostics']['candidate_reviews']['decisions'][0]['outcome'] == 'candidate_mismatch'
        # 原框、score、rank不变但mask变化，旧确认不可复用。
        masks = root/'changed-mask.rle'
        with masks.open('wb') as stream:
            stream.write(b'DOCOCR_MASK_RLE_V1\n' + struct.pack('<III', 300, 200, 200))
            stream.write(struct.pack('<IIII', 1, 2, 1, 39999))
            for _ in range(299):
                stream.write(struct.pack('<III', 0, 1, 40000))
        changed = dict(trace, mask_rle_path=str(masks))
        _, retained, _ = execute(cli, root/'changed-mask', image, setting, changed)
        assert len(retained['pages'][0]['blocks']) == 2
        assert retained['layout_diagnostics']['candidate_reviews']['decisions'][0]['outcome'] == 'candidate_mismatch'
        # 字段正确但对象是文字、页眉图、图表、印章或未知类，仍保护。
        for label in [3, 5, 7, 9, 12, 13, 16, 20, 21, 22, 99]:
            protected = [label, .95, 30, 200, 90, 240, 0]
            trial = copy.deepcopy(setting)
            trial['execution']['layout_candidate_reviews'] = [review(image, protected)]
            text = '<table><tr><td>有效内容</td></tr></table>' if label == 21 else 'A'
            case = dict(candidates=[protected], outputs=[dict(bbox=protected[2:6], text=text)])
            job, retained, _ = execute(cli, root/f'protected-{label}', image, trial, case)
            assert len(retained['pages'][0]['blocks']) == 1
            assert retained['layout_diagnostics']['candidate_reviews']['decisions'][0]['outcome'] == 'protected_content'
            reexport(production, job, root/f'protected-{label}-reexport')
        # 原始 image 类可显式确认装饰；这与 NMS 及识别失败分开记账。
        trial = copy.deepcopy(setting)
        trial['execution']['layout_candidate_reviews'][0]['decision'] = 'confirmed_decoration'
        _, decoration, _ = execute(cli, root/'decoration', image, trial, trace)
        assert decoration['layout_diagnostics']['candidates'][0]['filter_reason'] == 'review_confirmed_decoration'
        # 空数组是证据收集模式，不改变内容或顺序；辅助工具生成相同复核绑定。
        trial['execution']['layout_candidate_reviews'] = []
        job, collected, _ = execute(cli, root/'collect', image, trial, trace)
        assert collected['pages'] == old['pages']
        generated = root/'review.json'
        subprocess.run([sys.executable, str(Path(__file__).resolve().parents[1]/'scripts/layout_candidate_review.py'),
                        '--job', str(job), '--candidate', '0', '--decision', 'confirmed_watermark',
                        '--reason', review(image, row)['reason'], '--out', str(generated)], check=True)
        assert json.loads(generated.read_text()) == review(image, row)
        # 非法配置在作业开始前拒绝。
        for index, mutate in enumerate([
            lambda r: r.update(candidate_id=-1), lambda r: r.update(candidate_id=300),
            lambda r: r.update(candidate_id=True), lambda r: r.update(candidate_id=0.5),
            lambda r: r.update(reason='   '), lambda r: r.update(decision='skip'),
            lambda r: r.update(page_rgb_sha256=''), lambda r: r.update(candidate_sha256='X'*64),
        ]):
            trial = copy.deepcopy(setting)
            mutate(trial['execution']['layout_candidate_reviews'][0])
            cfg = root/f'invalid-{index}.json'
            cfg.write_text(json.dumps(trial))
            result = subprocess.run([cli, '--config', str(cfg), '--input', str(image), '--out', str(root/f'invalid-{index}')], capture_output=True)
            assert result.returncode != 0
        # 离线导出不能遗失 skip 的证据。
        for index, mutate in enumerate([
            lambda d: d['layout_diagnostics'].pop('candidate_reviews'),
            lambda d: d['layout_diagnostics']['candidate_reviews']['decisions'][0].update(outcome='retained'),
            lambda d: d['layout_diagnostics']['candidate_reviews']['decisions'][0].update(decision='suspected'),
            lambda d: d['layout_diagnostics']['candidate_reviews']['decisions'][0].update(crop_asset=None),
        ]):
            bad = copy.deepcopy(doc)
            mutate(bad)
            reexport(production, after, root/f'bad-{index}', bad, False)
        # 多页 PDF 保留每页复核记录、原页资源与统一 1.12 导出契约。
        pdf = root/'pages.pdf'
        frame = Image.new('RGB', (800, 1200), 'white')
        frame.save(pdf, save_all=True, append_images=[Image.new('RGB', (800, 1200), 'lightgray')])
        job, pdf_doc, _ = execute(cli, root/'pdf', pdf, setting, trace)
        assert pdf_doc['schema_version'] == '1.12' and len(pdf_doc['pages']) == 2
        for page in pdf_doc['pages']:
            assert len(page['blocks']) == 2
            r = page['layout_diagnostics']['candidate_reviews']
            assert r['decisions'][0]['outcome'] == 'page_mismatch'
            assert r['source_asset'] is None
        reexport(production, job, root/'pdf-reexport')
        # 同一公共 C ABI 作业直接核验（与 CLI 使用同一契约）。
        if len(sys.argv) > 3:
            from issue22_retry import Jobs
            fixture_path = root/'abi-trace.json'
            fixture_path.write_text(json.dumps(trace))
            previous = os.environ.get('DOCOCR_TEST_STRUCTURE_PATH')
            os.environ['DOCOCR_TEST_STRUCTURE_PATH'] = str(fixture_path)
            jobs = Jobs(sys.argv[3], setting, image.read_bytes())
            handle = jobs.create()
            try:
                import ctypes
                assert jobs.lib.dococr_job_run(handle, ctypes.byref(jobs.input)) == 0
                abi_doc, _ = jobs.save(handle, root/'abi')
                assert abi_doc == doc
            finally:
                jobs.lib.dococr_job_destroy(handle)
                jobs.close()
                if previous is None:
                    os.environ.pop('DOCOCR_TEST_STRUCTURE_PATH', None)
                else:
                    os.environ['DOCOCR_TEST_STRUCTURE_PATH'] = previous
        print('explicit review: PASS skip, retention, stale evidence, protected labels, config, reexport and C ABI')


if __name__ == '__main__':
    main()
