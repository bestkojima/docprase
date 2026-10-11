"""公共 CLI 验证并排示意图及侧边选项的识别前归属。"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from PIL import Image
from printed_page_integration import ROOT, config


def check(fixture_cli, production_cli, root, name, candidates, outputs, expected, groups):
    folder = root / name
    folder.mkdir()
    image = folder / 'page.png'
    Image.new('RGB', (1664, 2340), 'white').save(image)
    setting = config('printed_page_structure')
    setting['execution'].update(layout_preprocess='reference', layout_score_threshold=.3)
    cfg, trace, job = folder / 'config.json', folder / 'fixture.json', folder / 'job'
    cfg.write_text(json.dumps(setting))
    trace.write_text(json.dumps(dict(candidates=candidates, outputs=outputs)))
    result = subprocess.run([fixture_cli, '--config', str(cfg), '--input', str(image), '--out', str(job)],
        env=dict(os.environ, DOCOCR_TEST_STRUCTURE_PATH=str(trace)), cwd=ROOT, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    document = json.loads((job / 'document.json').read_text())
    page = document['pages'][0]
    layouts = {b['id']: b for b in page['layout_blocks']}
    regions = {r['id']: r for r in page['regions']}
    candidate = {b['id']: layouts[regions[b['source_region_ids'][0]]['source_layout_block_ids'][0]]['candidate_id']
                 for b in page['blocks']}
    actual = [candidate[bid] for bid in page['reading_order']]
    if expected is not None:
        assert actual == expected, (name, actual, expected)
    plan = page['structure_plan']
    actual_groups = [[(candidate[m['image_block_id']], candidate[m['caption_block_id']] if m['caption_block_id'] else None)
                      for m in group['items']] for group in plan['groups']]
    assert actual_groups == groups, (name, actual_groups, groups)
    assert plan['stage'] == 'before_recognition' and plan['block_order'] == page['reading_order']
    manifest = json.loads((job / 'run-manifest.json').read_text())
    assert [r['request_id'] for r in manifest['regions']] == ['req' + rid for rid in plan['recognition_order']]
    result = subprocess.run([production_cli, '--reexport', str(job / 'document.json'), '--asset-root', str(job),
                             '--out', str(folder / 'reexport')], cwd=ROOT, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    for name in ('document.json', 'document.md'):
        assert (folder / 'reexport' / name).read_bytes() == (job / name).read_bytes()


def main():
    with tempfile.TemporaryDirectory(prefix='dococr-issue27-structure-') as temporary:
        root = Path(temporary)
        # 同一横排的顶部有偏差，图注基线相同；短引导文字不覆盖整排。
        candidates = [[22, .95, 90, 900, 370, 940, 0],
            [14, .95, 130, 1050, 660, 1370, 1], [14, .95, 800, 1010, 1380, 1370, 2],
            [7, .95, 288, 1400, 414, 1440, 3], [7, .95, 1028, 1402, 1188, 1442, 4]]
        outputs = [dict(bbox=row[2:6], text=text) for row, text in
                   zip((candidates[0], candidates[3], candidates[4]), ('示意图', '图一', '图二'))]
        check(*sys.argv[1:3], root, 'captioned-pair', candidates, outputs,
              [0, 1, 3, 2, 4], [[(1, 3), (2, 4)]])
        columns = [[22, .95, 90, 900, 650, 990, 0], [22, .95, 800, 900, 1380, 990, 1],
            [14, .95, 130, 1050, 660, 1370, 2], [14, .95, 800, 1010, 1380, 1370, 3],
            [7, .95, 288, 1400, 414, 1440, 4], [7, .95, 1028, 1402, 1188, 1442, 5]]
        outputs = [dict(bbox=row[2:6], text='独立栏目说明') for row in columns if row[0] != 14]
        check(*sys.argv[1:3], root, 'independent-columns', columns, outputs, None, [])
        options = [[22, .95, 89, 1800, 1543, 1915, 0],
            [14, .95, 156, 1993, 386, 2058, 1], [14, .95, 496, 1994, 769, 2089, 2],
            [14, .95, 854, 1990, 1099, 2100, 3], [14, .95, 1203, 1995, 1423, 2089, 4],
            [7, .95, 92, 2056, 138, 2095, 5], [7, .95, 444, 2057, 482, 2095, 6],
            [7, .95, 794, 2057, 838, 2095, 7], [7, .95, 1143, 2056, 1189, 2095, 8]]
        outputs = [dict(bbox=options[0][2:6], text='选择示意图。')]
        outputs += [dict(bbox=row[2:6], text=letter) for row, letter in zip(options[5:], 'ABCD')]
        check(*sys.argv[1:3], root, 'lower-corner-labels', options, outputs,
              [0, 1, 5, 2, 6, 3, 7, 4, 8], [[(1, 5), (2, 6), (3, 7), (4, 8)]])


if __name__ == '__main__':
    main()
