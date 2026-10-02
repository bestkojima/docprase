"""公共 CLI：重叠文字行只识别一次，页码不制造分栏，保持公式预算兼容。"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from PIL import Image
import jsonschema
from printed_page_integration import ROOT, config


def run(root, name, rows, outputs, size=(640, 640)):
    folder = root / name
    folder.mkdir()
    image = folder / 'input.png'
    Image.new('RGB', size, 'white').save(image)
    cfg = config('printed_page_structure')
    cfg['execution'].update(layout_preprocess='reference', layout_score_threshold=.3)
    (folder / 'config.json').write_text(json.dumps(cfg))
    trace = folder / 'trace.json'
    trace.write_text(json.dumps(dict(candidates=rows, outputs=outputs)))
    job = folder / 'job'
    subprocess.run([sys.argv[1], '--config', str(folder / 'config.json'), '--input', str(image),
                    '--out', str(job)], cwd=ROOT, env=dict(os.environ, DOCOCR_TEST_STRUCTURE_PATH=str(trace)),
                   capture_output=True, check=True)
    d = json.loads((job / 'document.json').read_text())
    jsonschema.validate(d, json.loads((ROOT / f"schemas/document-ir/document-ir-{d['schema_version']}-image.schema.json").read_text()))
    reexport = folder / 'reexport'
    subprocess.run([sys.argv[2], '--reexport', str(job / 'document.json'), '--asset-root', str(job),
                    '--out', str(reexport)], cwd=ROOT, capture_output=True, check=True)
    assert (reexport / 'document.md').read_bytes() == (job / 'document.md').read_bytes()
    assert (reexport / 'document.json').read_bytes() == (job / 'document.json').read_bytes()
    return d


def main():
    with tempfile.TemporaryDirectory(prefix='dococr-input-planning-') as temp:
        root = Path(temp)
        failures = []
        for name, case in [('overlap-top', ([135, 1769, 1230, 2106], [139, 1753, 1029, 1791])),
                           ('overlap-bottom', ([1457, 992, 2454, 1106], [1464, 1083, 2394, 1121]))]:
            parent, line = case
            union = [min(parent[0], line[0]), min(parent[1], line[1]), max(parent[2], line[2]), max(parent[3], line[3])]
            for reverse in (False, True):
                rows = [[22, .95, *parent, 0], [22, .9, *line, 1]]
                if reverse:
                    rows.reverse()
                d = run(root, name + str(reverse), rows,
                        [dict(bbox=parent, text='半行'), dict(bbox=line, text='重复行'),
                         dict(bbox=union, text='完整正文，出现一次。')], (2867, 2339))
                page = d['pages'][0]
                if len(page['blocks']) != 1:
                    failures.append(name + ': 同一文字行仍被重复识别')
                    continue
                b = page['blocks'][0]
                assert b['bbox'] == union and b['content']['text'] == '完整正文，出现一次。'
                assert len(page['layout_blocks']) == 2 and len(page['regions'][0]['source_layout_block_ids']) == 2
                assert len(page['structure_plan']['recognition_order']) == 1
                assert len(b['provenance']['recognition']['attempts']) == 1
                assert any(c['handling_reason'] == 'overlapping_text_owned_by_text' for c in d['layout_diagnostics']['candidates'])
        for name, rows in [
            ('adjacent-prose', [[22,.95,40,20,500,40,0], [22,.9,40,40,500,220,1]]),
            ('separate-columns', [[22,.95,40,20,280,220,0], [22,.9,300,20,540,50,1]]),
            ('heading-is-not-body', [[22,.95,40,30,500,220,0], [17,.9,40,20,500,45,1]]),
            ('ambiguous-owner', [[22,.95,40,0,300,110,0], [22,.9,40,100,300,310,1],
                                 [22,.85,50,95,290,120,2]]),
        ]:
            d = run(root, name, rows, [dict(bbox=row[2:6], text=f'独立内容{i}') for i,row in enumerate(rows)])
            assert len(d['pages'][0]['blocks']) == len(rows), name
            assert all(c['handling_reason'] != 'overlapping_text_owned_by_text' for c in d['layout_diagnostics']['candidates']), name
        first, second, footer = [176,314,346,329], [178,331,313,342], [563,605,584,614]
        d = run(root, 'page-number-no-column', [[22,.9,*first,222],[22,.9,*second,269],[16,.9,*footer,299]],
                [dict(bbox=first,text='(i) 第一条件'),dict(bbox=second,text='(ii) 第二条件'),dict(bbox=footer,text='211')])
        text = [b['content']['text'] for b in d['pages'][0]['blocks'] if b['type']=='text']
        if text.index('(i) 第一条件') > text.index('(ii) 第二条件'):
            failures.append('页码制造假分栏，反应条件顺序颠倒')
        bounds = [10, 10, 335, 310]
        d = run(root, 'formula-visual-budget', [[5,.95,*bounds,0]], [dict(bbox=bounds,text='$2^{64}-1$')])
        attempt = d['pages'][0]['blocks'][0]['provenance']['recognition']['attempts'][0]
        if attempt['config']['visual_min_pixels'] != 65536 or attempt['config']['visual_max_pixels'] != 313600:
            failures.append('初次公式识别使用了未经整体质量验证的视觉预算')
        assert not failures, failures
    print('input planning: PASS')


if __name__ == '__main__':
    main()
