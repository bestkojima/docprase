"""从公共作业入口复现横排选项错序，核验识别前计划和离线导出。"""
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from PIL import Image
from printed_page_integration import ROOT, config


def fixture():
    rows, outputs = [], []
    for question, top in [(17, 130), (18, 410)]:
        stem = [40, top - 80, 950, top - 35]
        rows.append([22, .95, *stem, len(rows)])
        outputs.append(dict(bbox=stem, text=f'{question}. 选择正确图像。'))
        for i, left in enumerate([50, 280, 475, 720]):
            rows.append([14, .95, left, top + (i == 1), left + 160, top + 140, len(rows)])
        for i, left in enumerate([115, 345, 540, 785]):
            bounds = [left, top + 155, left + 25, top + 175]
            # 第一题只有 A 为 figure_title，其余标签是普通正文。
            rows.append([7 if question == 18 or i == 0 else 22, .9, *bounds, len(rows)])
            outputs.append(dict(bbox=bounds, text='ABCD'[i] + '.'))
    return dict(candidates=rows, outputs=outputs)


def invoke(fixture_cli, production_cli, root, name, value, expected=None, caption_rows=None, ambiguous=()):
    folder = root / name
    folder.mkdir()
    image = folder / 'page.png'
    Image.new('RGB', (1000, 1400), 'white').save(image)
    setting = config('printed_page_structure')
    setting['execution'].update(layout_preprocess='reference', layout_score_threshold=.3)
    cfg = folder / 'config.json'
    cfg.write_text(json.dumps(setting))
    trace = folder / 'fixture.json'
    trace.write_text(json.dumps(value))
    env = dict(os.environ, DOCOCR_TEST_STRUCTURE_PATH=str(trace))
    job = folder / 'job'
    result = subprocess.run([fixture_cli, '--config', str(cfg), '--input', str(image), '--out', str(job)],
                            env=env, cwd=ROOT, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    d = json.loads((job / 'document.json').read_text())
    page = d['pages'][0]
    layouts = {b['id']: b for b in page['layout_blocks']}
    regions = {r['id']: r for r in page['regions']}
    candidate = {b['id']: layouts[regions[b['source_region_ids'][0]]['source_layout_block_ids'][0]]['candidate_id']
                 for b in page['blocks']}
    actual = [candidate[bid] for bid in page['reading_order']]
    if expected is None:
        expected = [0, 1, 5, 2, 6, 3, 7, 4, 8, 9, 10, 14, 11, 15, 12, 16, 13, 17]
    if caption_rows is None:
        caption_rows = [[5, 6, 7, 8], [14, 15, 16, 17]]
    assert actual == expected, f'{name}: expected={expected}, actual={actual}'
    assert d['schema_version'] == '1.10'
    plan = page['structure_plan']
    assert plan['stage'] == 'before_recognition'
    assert plan['block_order'] == page['reading_order']
    assert plan['region_order'] == [next(b for b in page['blocks'] if b['id'] == bid)['source_region_ids'][0]
                                    for bid in page['reading_order']]
    assert len(plan['groups']) == 2
    assert [[candidate[item['image_block_id']] for item in g['items']] for g in plan['groups']] == [
        [1, 2, 3, 4], [10, 11, 12, 13]]
    assert [[candidate[item['caption_block_id']] if item['caption_block_id'] else None
             for item in g['items']] for g in plan['groups']] == caption_rows
    assert {candidate[bid] for bid in plan['ambiguous_caption_ids']} == set(ambiguous)
    links = {(candidate[r['source_block_id']], candidate[r['target_block_id']])
             for r in page['relations'] if r['type'] == 'caption_of'}
    assert links == {(cap, img) for caps, imgs in zip(caption_rows, [[1, 2, 3, 4], [10, 11, 12, 13]])
                     for cap, img in zip(caps, imgs) if cap is not None}
    manifest = json.loads((job / 'run-manifest.json').read_text())
    assert [r['request_id'] for r in manifest['regions']] == ['req' + rid for rid in plan['recognition_order']]
    md = (job / 'document.md').read_text()
    blocks = {b['id']: b for b in page['blocks']}
    for bid in plan['ambiguous_caption_ids']:
        assert f'![原图]({blocks[bid]["content"]["resource"]})' in md
    assert md.count('图注与图片的对应关系尚未确认，请核对原图。') == len(ambiguous)
    for g in plan['groups']:
        for item in g['items']:
            if item['caption_block_id'] is None:
                continue
            image_block = blocks[item['image_block_id']]
            caption = blocks[item['caption_block_id']]
            if caption['status'] == 'ok':
                rendered = caption['content']['text']
            else:
                assert caption['status'] == 'failed'
                assert caption['content']['text'] == ''
                rendered = f'![原图]({caption["content"]["resource"]})'
                assert f'[识别失败：{caption["id"]}]' in md
                assert 'UNTRUSTED_LABEL' not in md
            fragment = f'![插图]({image_block["content"]["resource"]})\n\n{rendered}'
            assert fragment in md, fragment
    result = subprocess.run([production_cli, '--reexport', str(job / 'document.json'),
                             '--asset-root', str(job), '--out', str(folder / 'reexport')],
                            cwd=ROOT, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert (folder / 'reexport/document.md').read_bytes() == (job / 'document.md').read_bytes()
    assert json.loads((folder / 'reexport/document.json').read_text()) == d
    # 计划引用或顺序被篡改必须拒绝，不静默猜测新关系。
    bad = copy.deepcopy(d)
    bad['pages'][0]['structure_plan']['region_order'].reverse()
    saved = folder / 'bad-plan.json'
    saved.write_text(json.dumps(bad))
    result = subprocess.run([production_cli, '--reexport', str(saved), '--asset-root', str(job),
                             '--out', str(folder / 'bad-export')], cwd=ROOT, capture_output=True, text=True)
    assert result.returncode != 0
    print(f'{name}: PASS ordering, binding, scheduling, assets and reexport', flush=True)


def main():
    with tempfile.TemporaryDirectory(prefix='dococr-issue26-') as temporary:
        root = Path(temporary)
        invoke(sys.argv[1], sys.argv[2], root, 'two-option-rows', fixture())
        jittered = fixture()
        for cid in (1, 2, 3, 4, 10, 11, 12, 13):
            jittered['candidates'][cid][3] += (cid % 3) - 1
        invoke(sys.argv[1], sys.argv[2], root, 'one-pixel-jitter', jittered)
        failed = fixture()
        failed['outputs'][3].update(text='', raw_output='UNTRUSTED_LABEL',
                                   finish_reason='failed', stop_reason='error', error='controlled_failure')
        invoke(sys.argv[1], sys.argv[2], root, 'failed-label-keeps-binding', failed)
        sides = fixture()
        for cid, top, output in [(5, 130, 1), (14, 410, 6)]:
            bounds = [20, top + 60, 45, top + 80]
            sides['candidates'][cid][0] = 22
            sides['candidates'][cid][2:6] = bounds
            sides['outputs'][output].update(bbox=bounds, text=f'({output})')
        for cid in (2, 3, 4, 11, 12, 13):
            sides['candidates'][cid][3] += 15
        invoke(sys.argv[1], sys.argv[2], root, 'side-label-and-offset-options', sides)
        overlaps = fixture()
        for cid, top, output in [(5, 130, 1), (14, 410, 6)]:
            bounds = [115, top + 139, 140, top + 159]
            overlaps['candidates'][cid][2:6] = bounds
            overlaps['outputs'][output]['bbox'] = bounds
        invoke(sys.argv[1], sys.argv[2], root, 'caption-edge-overlap', overlaps)
        ambiguous = fixture()
        extra = [160, 285, 185, 305]
        ambiguous['candidates'].append([7, .9, *extra, 18])
        ambiguous['outputs'].append(dict(bbox=extra, text='第二个候选标签'))
        invoke(sys.argv[1], sys.argv[2], root, 'competing-labels-stay-unresolved', ambiguous,
               expected=[0, 1, 2, 6, 3, 7, 4, 8, 5, 18, 9, 10, 14, 11, 15, 12, 16, 13, 17],
               caption_rows=[[None, 6, 7, 8], [14, 15, 16, 17]], ambiguous=(5, 18))


if __name__ == '__main__':
    main()
