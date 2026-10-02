"""公共入口验证三类 OCR、image 资源和单图文字关联。"""
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

import jsonschema
from PIL import Image
from printed_page_integration import ROOT, config


def run(fixture_cli, production_cli, root, name, label, bounds, linked):
    folder = root / name
    folder.mkdir()
    image_bounds = [142, 1084, 652, 1469] if name.startswith('odb07') else [877, 390, 1291, 561]
    rows = [[14, .95, *image_bounds, 0]]
    outputs = []
    if label is not None:
        rows.append([label, .95, *bounds, 1])
        outputs.append(dict(bbox=bounds, text='斐波那契' if label == 24 else '第8题'))
    image = folder / 'page.png'
    Image.new('RGB', (1664, 2340), 'white').save(image)
    setting = config('printed_page_structure')
    setting['execution'].update(layout_preprocess='reference', layout_score_threshold=.3)
    cfg, trace, job = folder / 'config.json', folder / 'fixture.json', folder / 'job'
    cfg.write_text(json.dumps(setting))
    trace.write_text(json.dumps(dict(candidates=rows, outputs=outputs)))
    result = subprocess.run([fixture_cli, '--config', str(cfg), '--input', str(image), '--out', str(job)],
                            env=dict(os.environ, DOCOCR_TEST_STRUCTURE_PATH=str(trace)),
                            cwd=ROOT, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    doc = json.loads((job / 'document.json').read_text())
    page = doc['pages'][0]
    pairs = [(r['source_block_id'], r['target_block_id']) for r in page['relations'] if r['type'] == 'caption_of']
    assert pairs == ([('b0002', 'b0001')] if linked else []), f'{name}: 图注关联={pairs}'
    resource = next(b for b in page['blocks'] if b['type'] == 'image')
    assert resource['status'] == 'ok' and resource['error'] is None, f'{name}: 正常图片被误记为未处理'
    assert resource['provenance']['recognition']['attempts'] == []
    assert resource['provenance']['assessment']['reason'] == 'resource_saved'
    assert doc['status'] == 'ok'
    assert doc['schema_version'] == '1.10'
    jsonschema.validate(doc, json.loads((ROOT / 'schemas/document-ir/document-ir-1.10-image.schema.json').read_text()))
    regions = {r['id']: r for r in page['regions']}
    for block in page['blocks']:
        assert regions[block['source_region_ids'][0]]['recognition_type'] == block['type']
    ocr_order = [b['source_region_ids'][0] for b in page['blocks'] if b['type'] in ('text', 'formula', 'table')]
    assert page['structure_plan']['recognition_order'] == ocr_order
    manifest = json.loads((job / 'run-manifest.json').read_text())
    assert [r['request_id'] for r in manifest['regions']] == ['req' + rid for rid in ocr_order]
    md = (job / 'document.md').read_text()
    assert f'![插图]({resource["content"]["resource"]})' in md
    assert (job / resource['content']['resource']).read_bytes().startswith(b'\x89PNG')
    if linked:
        caption = next(b for b in page['blocks'] if b['type'] == 'text')
        assert f'![插图]({resource["content"]["resource"]})\n\n{caption["content"]["text"]}' in md
    result = subprocess.run([production_cli, '--reexport', str(job / 'document.json'),
                             '--asset-root', str(job), '--out', str(folder / 'reexport')],
                            cwd=ROOT, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert (folder / 'reexport/document.md').read_bytes() == (job / 'document.md').read_bytes()
    for mutation in ('type', 'order', 'image_attempt'):
        bad = copy.deepcopy(doc)
        p = bad['pages'][0]
        if mutation == 'type':
            p['regions'][0]['recognition_type'] = 'text'
        elif mutation == 'order':
            p['structure_plan']['recognition_order'].insert(0, resource['source_region_ids'][0])
        else:
            p['blocks'][0]['provenance']['assessment']['finish_reason'] = 'completed'
        saved = folder / (mutation + '.json')
        saved.write_text(json.dumps(bad))
        result = subprocess.run([production_cli, '--reexport', str(saved), '--asset-root', str(job),
                                 '--out', str(folder / mutation)], cwd=ROOT, capture_output=True, text=True)
        assert result.returncode != 0, f'{name}: 未拒绝篡改 {mutation}'
    print(f'{name}: PASS', flush=True)


def main():
    with tempfile.TemporaryDirectory(prefix='dococr-issue26-mapping-') as temporary:
        root = Path(temporary)
        run(*sys.argv[1:3], root, 'odb07-vision-footnote', 24, [169, 1476, 596, 1565], True)
        run(*sys.argv[1:3], root, 'odb08-single-text', 22, [1026, 581, 1147, 619], True)
        run(*sys.argv[1:3], root, 'real-footnote', 10, [1026, 581, 1147, 619], False)
        run(*sys.argv[1:3], root, 'heading', 17, [1026, 581, 1147, 619], False)
        run(*sys.argv[1:3], root, 'offset-body', 22, [880, 581, 985, 619], False)
        run(*sys.argv[1:3], root, 'image-only', None, None, False)


if __name__ == '__main__':
    main()
