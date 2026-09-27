"""固定中文教材页的真实双模型、公式归属和LaTeX业务回归。"""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

from PIL import Image
import jsonschema


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / 'tests/fixtures/ovis/formula_book_page.png'
REFERENCE = ROOT / 'tests/fixtures/ovis/formula_book_page.manifest.json'


def intersection_over_reference(box, polygon):
    left, top, right, bottom = polygon[0], polygon[1], polygon[4], polygon[5]
    width = max(0, min(box[2], right) - max(box[0], left))
    height = max(0, min(box[3], bottom) - max(box[1], top))
    return width * height / ((right - left) * (bottom - top))


def main():
    binary, output = Path(sys.argv[1]), Path(sys.argv[2])
    output.mkdir(parents=True, exist_ok=True)
    reference = json.loads(REFERENCE.read_text())
    source_sha = hashlib.sha256(FIXTURE.read_bytes()).hexdigest()
    assert source_sha == reference['image_sha256']
    command = [str(binary), '--config', 'configs/printed-page.example.json',
               '--input', str(FIXTURE), '--out', str(output / 'job')]
    process = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=900)
    (output / 'cli.stdout.log').write_text(process.stdout)
    (output / 'cli.stderr.log').write_text(process.stderr)
    assert process.returncode == 0, (process.returncode, process.stderr)
    job = output / 'job'
    document = json.loads((job / 'document.json').read_text())
    schema = json.loads((ROOT / 'docs/issue-18/document-ir-1.5-image.schema.json').read_text())
    jsonschema.validate(document, schema)
    assert document['schema_version'] == '1.5'
    assert json.loads(json.dumps(document, ensure_ascii=False)) == document
    page = document['pages'][0]
    blocks = page['blocks']
    layouts = {item['id']: item for item in page['layout_blocks']}
    regions = {item['id']: item for item in page['regions']}
    owners = {item['id']: item for item in blocks}
    assert len(page['reading_order']) == len(blocks)
    ownership = [r for r in page['relations'] if r['type'] == 'content_owned_by']
    assert ownership
    for relation in page['relations']:
        if relation['type'] != 'content_owned_by':
            assert relation['source_block_id'] in owners
            assert relation['target_block_id'] in owners
            assert owners[relation['source_block_id']]['type'] == 'text'
            if relation['type'] == 'caption_of':
                assert owners[relation['target_block_id']]['type'] in ('image', 'table')
    for relation in ownership:
        child = layouts[relation['source_layout_block_id']]
        owner = owners[relation['owner_block_id']]
        assert child['label'] == 'formula' and owner['type'] == 'text'
        assert relation['source_layout_block_id'] in (
            regions[owner['source_region_ids'][0]]['source_layout_block_ids'])
        assert owner['bbox'][0] <= child['bbox'][0] <= child['bbox'][2] <= owner['bbox'][2]
        assert owner['bbox'][1] <= child['bbox'][1] <= child['bbox'][3] <= owner['bbox'][3]
        assert not any(block['type'] == 'formula' and block['bbox'] == child['bbox']
                       for block in blocks)
    with Image.open(FIXTURE) as source:
        source = source.convert('RGB')
        for relation in ownership:
            owner = owners[relation['owner_block_id']]
            crop = job / owner['content']['resource']
            with Image.open(crop) as exported:
                assert list(exported.convert('RGB').getdata()) == list(
                    source.crop(tuple(owner['bbox'])).getdata())
    for candidate in document['layout_diagnostics']['candidates']:
        if candidate['selected']:
            assert candidate['mask_asset']
            assert (job / candidate['mask_asset']).exists()
    markdown = (job / 'document.md').read_text()
    first_gt = next(item for item in reference['annotations']
                    if item['category_type'] == 'equation_isolated' and
                    380 < item['poly'][1] < 390)
    first = next(block for block in blocks if block['type'] == 'formula' and
                 intersection_over_reference(block['bbox'], first_gt['poly']) > 0.8)
    assert first['status'] == 'ok'
    assert first['content']['format'] == 'latex' and first['content']['display']
    assert re.sub(r'\s+', '', first['content']['text']) == re.sub(
        r'\s+', '', first_gt['latex'][2:-2])
    assert first['provenance']['raw_output'].strip().startswith('$$')
    assert markdown.count(first['content']['text']) == 1
    assert '$$\n' + first['content']['text'] + '\n$$' in markdown
    assert first['content']['text'] == json.loads(
        json.dumps(document, ensure_ascii=False))['pages'][0]['blocks'][
            blocks.index(first)]['content']['text']
    text_parent = next(block for block in blocks if block['type'] == 'text' and
                       '$r^{n}' in block['content']['text'])
    assert any(item['owner_block_id'] == text_parent['id'] for item in ownership)
    assert markdown.count(text_parent['content']['text']) == 1
    numbered = next(block for block in blocks if block['type'] == 'text' and
                    '1 写出特征方程' in block['content']['text'])
    assert numbered['status'] == 'ok'
    assert r'\tag{' not in markdown
    mixed_gt = next(item for item in reference['annotations']
                    if item['category_type'] == 'equation_isolated' and
                    item['poly'][1] > 2000)
    mixed = next(block for block in blocks if block['type'] == 'formula' and
                 intersection_over_reference(block['bbox'], mixed_gt['poly']) > 0.8)
    assert mixed['status'] == 'partial'
    assert mixed['error'] == 'invalid_formula_syntax'
    assert mixed['provenance']['raw_output'].endswith('则\n')
    assert (job / mixed['content']['resource']).exists()
    manifest = json.loads((job / 'run-manifest.json').read_text())
    assert manifest['actual_device'] == 'cpu'
    assert len(manifest['regions']) == len(blocks)
    second_gt = next(item for item in reference['annotations']
                     if item['category_type'] == 'equation_isolated' and
                     625 < item['poly'][1] < 630)
    second = next(block for block in blocks if block['type'] == 'formula' and
                  intersection_over_reference(block['bbox'], second_gt['poly']) > 0.8)
    second_exact = re.sub(r'\s+', '', second['content']['text']) == re.sub(
        r'\s+', '', second_gt['latex'][2:-2])
    summary = {
        'command': command, 'exit_code': process.returncode,
        'source_sha256': source_sha, 'annotation_sha256': reference['source_annotation_sha256'],
        'schema_version': document['schema_version'], 'status': document['status'],
        'structure_ownership_acceptance': 'passed',
        'formula_content_quality': 'partial' if not second_exact else 'passed',
        'layout_selected': sum(x['selected'] for x in
                               document['layout_diagnostics']['candidates']),
        'content_blocks': len(blocks), 'ownership_relations': len(ownership),
        'formula_blocks': sum(x['type'] == 'formula' for x in blocks),
        'first_formula_gt_exact_ignoring_whitespace': True,
        'second_formula_format_ok': second['status'] == 'ok',
        'second_formula_gt_exact_ignoring_whitespace': second_exact,
        'mixed_formula_partial': mixed['status'] == 'partial',
        'partial_text_blocks': [x['id'] for x in blocks
                                if x['type'] == 'text' and x['status'] == 'partial'],
    }
    (output / 'summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(summary, ensure_ascii=False))


if __name__ == '__main__':
    main()
