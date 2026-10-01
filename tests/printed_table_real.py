"""用独立标注比较两张真实教材页的表格结构与单元格内容。"""
from html.parser import HTMLParser
import hashlib
import json
from pathlib import Path
import re
import sys

import jsonschema
from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
ORDINARY = ROOT / 'tests/fixtures/ovis/source_page.jpg'
MERGED = ROOT / 'tests/fixtures/ovis/merged_table_book_page.png'


class ReferenceTable(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.rows = []
        self.cells = []
        self.current = None

    def handle_starttag(self, tag, attrs):
        if tag == 'tr':
            self.rows.append([])
        elif tag in ('td', 'th'):
            attrs = dict(attrs)
            self.current = {'row': len(self.rows)-1, 'rowspan': int(attrs.get('rowspan', 1)),
                            'colspan': int(attrs.get('colspan', 1)), 'header': tag == 'th',
                            'text': ''}
        elif tag == 'br' and self.current is not None:
            self.current['text'] += '\n'

    def handle_endtag(self, tag):
        if tag in ('td', 'th') and self.current is not None:
            self.cells.append(self.current)
            self.current = None

    def handle_data(self, data):
        if self.current is not None:
            self.current['text'] += data

    def grid(self):
        occupied = set()
        for cell in self.cells:
            row = cell['row']
            column = next(c for c in range(100) if (row, c) not in occupied)
            cell['column'] = column
            for r in range(row, row + cell['rowspan']):
                for c in range(column, column + cell['colspan']):
                    occupied.add((r, c))
        return {'rows': len(self.rows), 'columns': max(c for _, c in occupied)+1,
                'cells': self.cells}


def box_overlap(block, poly):
    left, top, right, bottom = poly[0], poly[1], poly[4], poly[5]
    b = block['bbox']
    area = max(0, min(right, b[2])-max(left, b[0])) * max(0, min(bottom, b[3])-max(top, b[1]))
    return area / ((right-left)*(bottom-top))


def compare(job, image, references, min_tables):
    document = json.loads((job / 'document.json').read_text())
    schema = json.loads((ROOT / 'docs/issue-22/document-ir-1.7-image.schema.json').read_text())
    jsonschema.validate(document, schema)
    assert document['schema_version'] == '1.7'
    assert json.loads(json.dumps(document, ensure_ascii=False)) == document
    page = document['pages'][0]
    blocks = page['blocks']
    layouts = {v['id']: v for v in page['layout_blocks']}
    regions = {v['id']: v for v in page['regions']}
    owners = {v['id']: v for v in blocks}
    assert len(page['reading_order']) == len(blocks)
    assert len({b['id'] for b in blocks}) == len(blocks)
    assert len(json.loads((job / 'run-manifest.json').read_text())['regions']) == len(blocks)
    for resource in document['resources']:
        assert (job / resource['path']).read_bytes().startswith(b'\x89PNG')
    for candidate in document['layout_diagnostics']['candidates']:
        if candidate['selected']:
            assert (job / candidate['mask_asset']).exists()
    with Image.open(image) as original:
        original = original.convert('RGB')
        for table in (b for b in blocks if b['type'] == 'table'):
            with Image.open(job / table['content']['resource']) as crop:
                assert list(crop.convert('RGB').getdata()) == list(
                    original.crop(tuple(table['bbox'])).getdata())
    markdown = (job / 'document.md').read_text()
    reports = []
    for reference in references:
        table = max((b for b in blocks if b['type'] == 'table'),
                    key=lambda b: box_overlap(b, reference['polygon']))
        assert box_overlap(table, reference['polygon']) > 0.85
        assert table['status'] == 'ok' and table['content']['format'] == 'html'
        assert table['content']['text'].startswith('<table>')
        assert table['provenance']['raw_output'].startswith('<table')
        assert markdown.count(table['content']['text']) == 1
        assert not re.search(r'<img\b', table['content']['text'], re.I)
        actual = table['content']['table']
        assert all(cell['bbox'] is None for cell in actual['cells'])
        assert all(cell['text'] for cell in actual['cells'])
        parser = ReferenceTable()
        # 该标注的数学内容有未转义的 '<' 比较符；只在参考侧转义它们，
        # 避免 HTMLParser 把公式当成标签吞掉，再独立比较行列结构。
        reference_html = re.sub(
            r'<(?!/?(?:table|tr|td|th|br|thead|tbody|tfoot)\b)',
            '&lt;', reference['html'])
        parser.feed(reference_html)
        expected = parser.grid()
        shape_equal = (actual['rows'], actual['columns']) == (
            expected['rows'], expected['columns'])
        actual_structure = [(c['row'], c['column'], c['rowspan'], c['colspan'], c['header'])
                            for c in actual['cells']]
        expected_structure = [(c['row'], c['column'], c['rowspan'], c['colspan'], c['header'])
                              for c in expected['cells']]
        structure_equal = shape_equal and actual_structure == expected_structure
        cell_differences = []
        for i, (got, want) in enumerate(zip(actual['cells'], expected['cells'])):
            if got['text'] != want['text']:
                cell_differences.append({'cell_index': i, 'actual': got['text'],
                                         'reference': want['text'],
                                         'equal_ignoring_whitespace':
                                         re.sub(r'\s+', '', got['text']) ==
                                         re.sub(r'\s+', '', want['text'])})
        assert len(actual['cells']) == len(expected['cells'])
        assert structure_equal, (table['id'], actual_structure, expected_structure)
        reports.append({'annotation_id': reference['annotation_id'], 'block_id': table['id'],
                        'bbox': table['bbox'], 'rows': actual['rows'],
                        'columns': actual['columns'], 'cells': len(actual['cells']),
                        'structure_exact': structure_equal,
                        'reference_math_less_than_escaped': reference_html != reference['html'],
                        'content_exact_cells': len(actual['cells'])-len(cell_differences),
                        'content_nonempty_cells': len(actual['cells']),
                        'cell_differences': cell_differences})
    assert len(reports) >= min_tables
    ownership = [r for r in page['relations'] if r['type'] == 'content_owned_by']
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
        if owner['type'] == 'table':
            assert child['label'] in ('text', 'formula')
            assert owner['bbox'][0] <= child['bbox'][0] <= child['bbox'][2] <= owner['bbox'][2]
            assert owner['bbox'][1] <= child['bbox'][1] <= child['bbox'][3] <= owner['bbox'][3]
            assert relation['source_layout_block_id'] in regions[
                owner['source_region_ids'][0]]['source_layout_block_ids']
            assert not any(block['bbox'] == child['bbox'] and block['type'] == child['label']
                           for block in blocks)
    return {'document_status': document['status'], 'layout_selected': sum(
        c['selected'] for c in document['layout_diagnostics']['candidates']),
        'content_blocks': len(blocks), 'table_ownership_relations': sum(
            owners[r['owner_block_id']]['type'] == 'table' for r in ownership),
        'tables': reports}


def main():
    ordinary_job, merged_job, ordinary_rgb, output = map(Path, sys.argv[1:])
    ordinary_manifest = json.loads((ROOT / 'tests/fixtures/ovis/manifest.json').read_text())
    merged_manifest = json.loads((ROOT / 'tests/fixtures/ovis/merged_table_book_page.manifest.json').read_text())
    assert hashlib.sha256(ORDINARY.read_bytes()).hexdigest() == next(
        s['source_page_sha256'] for s in ordinary_manifest['samples']
        if s['name'] == 'complete_table')
    with Image.open(ORDINARY) as source, Image.open(ordinary_rgb) as normalized:
        assert list(source.convert('RGB').getdata()) == list(normalized.convert('RGB').getdata())
    assert hashlib.sha256(MERGED.read_bytes()).hexdigest() == merged_manifest['image_sha256']
    annotation_file = ROOT / 'output/ovis-source/OmniDocBench.json'
    full_annotation_sha_verified = annotation_file.exists()
    if full_annotation_sha_verified:
        assert hashlib.sha256(annotation_file.read_bytes()).hexdigest() == (
            merged_manifest['source_annotation_sha256'])
    ordinary_reference = (ROOT / 'tests/fixtures/ovis/complete_table.reference.txt').read_text()
    ordinary_sample = next(s for s in ordinary_manifest['samples'] if s['name'] == 'complete_table')
    summary = {
        'ordinary': compare(ordinary_job, ordinary_rgb, [{
            'annotation_id': ordinary_sample['annotation_id'],
            'polygon': ordinary_sample['source_polygon'],
            'html': ordinary_reference}], 1),
        'merged': compare(merged_job, MERGED, merged_manifest['tables'], 2),
        'source_sha256': {'ordinary': hashlib.sha256(ORDINARY.read_bytes()).hexdigest(),
                          'merged': merged_manifest['image_sha256']},
        'annotation_sha256': merged_manifest['source_annotation_sha256'],
        'full_annotation_sha_verified': full_annotation_sha_verified,
    }
    assert summary['merged']['table_ownership_relations'] >= 1
    merged_document = json.loads((merged_job / 'document.json').read_text())
    assert any(b['type'] == 'text' and '表 E.0.2-1' in b['content']['text']
               for b in merged_document['pages'][0]['blocks'])
    assert (merged_job / 'document.md').read_text().count('表 E.0.2-1') == 1
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(summary, ensure_ascii=False))


if __name__ == '__main__':
    main()
