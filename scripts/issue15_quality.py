"""Report frozen #15 quality and all non-ok blocks without hiding failures."""
import ast
import hashlib
import json
from html.parser import HTMLParser
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]


def frozen_pdf_reference():
    """Read the literal truth from #11 without importing its optional deps."""
    tree = ast.parse((ROOT / 'tests/pdf_real.py').read_text())
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Name) and target.id == 'GROUND_TRUTH'
                for target in node.targets):
            return ast.literal_eval(node.value)
    raise ValueError('tests/pdf_real.py lacks literal GROUND_TRUTH')


GROUND_TRUTH = frozen_pdf_reference()


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

ANCHORS = [
    'Printed Science Practice - Form A', 'Section 1: Measurement',
    '1. Measure the length of a pencil.', 'Record the answer in centimeters.',
    '2. A meter has one hundred', 'centimeters. Convert 2 m to cm.',
    '3. Explain why repeated readings', 'can reduce random error.',
    '4. The chart shows two readings.', 'Which reading is larger?',
    'Figure 1. Recorded readings.', '5. Write the result as a sentence.',
    'Include the unit in your answer.', 'Section 2: Short answer',
    '6. State one reason to check a measurement twice.', 'End of practice page.',
]


def normalize(value):
    return re.sub(r'\s+', ' ', value.strip())


def edit_distance(a, b):
    previous = list(range(len(b) + 1))
    for index, left in enumerate(a, 1):
        current = [index]
        for column, right in enumerate(b, 1):
            current.append(min(current[-1] + 1, previous[column] + 1,
                               previous[column - 1] + (left != right)))
        previous = current
    return previous[-1]


def overlap(box, poly):
    left, top, right, bottom = poly[0], poly[1], poly[4], poly[5]
    intersection = max(0, min(box[2], right) - max(box[0], left)) * max(
        0, min(box[3], bottom) - max(box[1], top))
    return intersection / ((right - left) * (bottom - top))


def blocks(result):
    return [b for page in result['pages'] for b in page['blocks']]


def non_ok(result, manifest):
    region = {r['request_id']: r for r in manifest['regions']}
    return [{'block_id': b['id'], 'type': b['type'], 'status': b['status'],
             'error': b.get('error'),
             'stop_reason': region.get(b['provenance']['request_id'], {}).get('stop_reason'),
             'raw_sha256': hashlib.sha256(b['provenance']['raw_output'].encode()).hexdigest(),
             'raw_length': len(b['provenance']['raw_output']),
             'raw_in': 'job/document.json'}
            for b in blocks(result) if b['status'] != 'ok']


def text_reference(actual, reference):
    got, want = normalize(actual), normalize(reference)
    return {'exact': got == want, 'edit_distance': edit_distance(got, want),
            'reference_chars': len(want),
            'cer': edit_distance(got, want) / len(want) if want else None,
            'actual': actual, 'reference': reference}


def pdf_quality(result):
    rows = []
    for page_index, expected in enumerate(GROUND_TRUTH):
        page = result['pages'][page_index] if page_index < len(result['pages']) else None
        actual = ([b['content']['text'] for b in page['blocks'] if b['type'] == 'text']
                  if page is not None else [])
        for i, want in enumerate(expected):
            # Frozen page is authored as five independent lines. Missing line
            # is an explicit deletion; an extra block is reported separately.
            match = text_reference(actual[i] if i < len(actual) else '', want)
            match.update({'page': page['page_id'] if page else f'missing-page-{page_index+1}',
                          'line': i + 1})
            rows.append(match)
        if len(actual) > len(expected):
            rows.append({'page': page['page_id'], 'extra_text_blocks': actual[len(expected):]})
    for page in result['pages'][len(GROUND_TRUTH):]:
        rows.append({'page': page['page_id'], 'extra_page': True,
                     'extra_text_blocks': [b['content']['text'] for b in page['blocks']
                                           if b['type'] == 'text']})
    return {'lines': rows, 'exact_lines': sum(x.get('exact', False) for x in rows),
            'reference_lines': sum(map(len, GROUND_TRUTH)),
            'total_edit_distance': sum(x.get('edit_distance', 0) for x in rows),
            'total_reference_chars': sum(x.get('reference_chars', 0) for x in rows)}


def formula_quality(result):
    reference = json.loads((ROOT / 'tests/fixtures/ovis/formula_book_page.manifest.json').read_text())
    formulas = [b for b in blocks(result) if b['type'] == 'formula']
    checks = []
    for ref in [a for a in reference['annotations'] if a['category_type'] == 'equation_isolated']:
        ranked = sorted(((overlap(b['bbox'], ref['poly']), b) for b in formulas),
                        key=lambda x: x[0], reverse=True)
        score, found = ranked[0] if ranked else (0, None)
        if score <= 0.8:
            checks.append({'reference_y': ref['poly'][1], 'matched': False,
                           'reference': ref['latex']})
            continue
        actual = found['content']['text']
        want = ref['latex'][2:-2] if ref['latex'].startswith('$$') and ref['latex'].endswith('$$') else ref['latex']
        checks.append({'reference_y': ref['poly'][1], 'matched': True,
                       'overlap_over_reference': score, 'block_id': found['id'],
                       'status': found['status'], 'format': found['content']['format'],
                       'exact_ignoring_whitespace': re.sub(r'\s+', '', actual) ==
                           re.sub(r'\s+', '', want), 'actual': actual,
                       'reference': want, 'raw': found['provenance']['raw_output']})
    return {'independent': checks,
            'ownership_count': sum(r['type'] == 'content_owned_by' for p in result['pages']
                                   for r in p['relations'])}


def table_quality(result, references):
    page = result['pages'][0]
    tables = [b for b in page['blocks'] if b['type'] == 'table']
    reports = []
    for ref in references:
        ranked = sorted(((overlap(b['bbox'], ref['polygon']), b) for b in tables),
                        key=lambda x: x[0], reverse=True)
        score, table = ranked[0] if ranked else (0, None)
        if score <= 0.5:
            reports.append({'annotation_id': ref['annotation_id'], 'matched': False,
                            'reference_html': ref['html']})
            continue
        reference_html = re.sub(
            r'<(?!/?(?:table|tr|td|th|br|thead|tbody|tfoot)\b)', '&lt;', ref['html'])
        parser = ReferenceTable()
        parser.feed(reference_html)
        expected = parser.grid()
        actual = table['content'].get('table')
        if actual is None:
            reports.append({'annotation_id': ref['annotation_id'], 'matched': True,
                            'block_id': table['id'], 'status': table['status'],
                            'structure': None, 'raw': table['provenance']['raw_output']})
            continue
        fields = ('row', 'column', 'rowspan', 'colspan', 'header')
        got = [tuple(c[k] for k in fields) for c in actual['cells']]
        want = [tuple(c[k] for k in fields) for c in expected['cells']]
        cells = []
        for i in range(max(len(actual['cells']), len(expected['cells']))):
            a = actual['cells'][i]['text'] if i < len(actual['cells']) else ''
            b = expected['cells'][i]['text'] if i < len(expected['cells']) else ''
            cells.append({'index': i, 'exact': normalize(a) == normalize(b),
                          'actual': a, 'reference': b})
        reports.append({'annotation_id': ref['annotation_id'], 'matched': True,
                        'block_id': table['id'], 'status': table['status'],
                        'rows': actual['rows'], 'columns': actual['columns'],
                        'reference_rows': expected['rows'], 'reference_columns': expected['columns'],
                        'structure_exact': (actual['rows'], actual['columns'], got) ==
                           (expected['rows'], expected['columns'], want),
                        'cell_exact': sum(c['exact'] for c in cells),
                        'cell_count': len(cells), 'cell_differences': [c for c in cells if not c['exact']]})
    return reports


def anchors_quality(result, anchors):
    ordered = []
    for page_index, page in enumerate(result['pages']):
        by_id = {b['id']: b for b in page['blocks']}
        ordered.extend((page_index, order_index, by_id[block_id])
                       for order_index, block_id in enumerate(page['reading_order']))
    observed = []
    for anchor in anchors:
        locations = [(page_index, order_index, hit.start())
                     for page_index, order_index, block in ordered
                     for hit in re.finditer(re.escape(anchor), block['provenance']['raw_output'])]
        observed.append({'anchor': anchor, 'locations': locations,
                         'found': bool(locations), 'unique': len(locations) == 1})
    pair_count = 0
    pair_correct = 0
    wrong = []
    for i in range(len(observed)):
        for j in range(i+1, len(observed)):
            if observed[i]['unique'] and observed[j]['unique']:
                pair_count += 1
                if tuple(observed[i]['locations'][0]) < tuple(observed[j]['locations'][0]):
                    pair_correct += 1
                else:
                    wrong.append([observed[i]['anchor'], observed[j]['anchor']])
    return {'anchors': observed, 'pair_correct': pair_correct,
            'pair_decidable': pair_count, 'wrong_pairs': wrong,
            'missing': [x['anchor'] for x in observed if not x['found']]}


def main():
    output = Path(sys.argv[1]).resolve()
    docs = {}
    report = {'samples': {}}
    for sample in json.loads((ROOT / 'docs/issue-15/samples.json').read_text())['samples']:
        name = sample['id']
        job = output / name / 'job'
        if not (job / 'document.json').exists():
            report['samples'][name] = {'missing_job': True}
            continue
        document = json.loads((job / 'document.json').read_text())
        manifest = json.loads((job / 'run-manifest.json').read_text())
        docs[name] = document
        diagnostics = document.get('layout_diagnostics', {})
        candidates = diagnostics.get('candidates', [])
        report['samples'][name] = {'document_status': document['status'],
            'block_statuses': {status: sum(b['status'] == status for b in blocks(document))
                               for status in ('ok', 'partial', 'failed', 'skipped')},
            'layout_candidate_count': len(candidates),
            'layout_filtered_count': sum(not c['selected'] for c in candidates),
            'unknown_blocks': [b['id'] for b in blocks(document) if b['type'] == 'unknown'],
            'non_ok_blocks': non_ok(document, manifest),
            'stop_reasons': {reason: sum(r['stop_reason'] == reason for r in manifest['regions'])
                             for reason in sorted({r['stop_reason'] for r in manifest['regions']})}}
    if 'zh_text_table' in docs:
        expected = (ROOT / 'tests/fixtures/ovis/chinese_text.reference.txt').read_text()
        candidates = [b for b in blocks(docs['zh_text_table']) if b['type'] == 'text']
        scored = sorted((text_reference(b['provenance']['raw_output'], expected) | {'block_id': b['id']}
                         for b in candidates), key=lambda x: x['edit_distance'])
        report['samples']['zh_text_table']['text_excerpt'] = scored[0] if scored else None
        refs = json.loads((ROOT / 'tests/fixtures/ovis/manifest.json').read_text())
        ref = next(s for s in refs['samples'] if s['name'] == 'complete_table')
        report['samples']['zh_text_table']['tables'] = table_quality(docs['zh_text_table'], [{
            'annotation_id': ref['annotation_id'], 'polygon': ref['source_polygon'],
            'html': (ROOT / 'tests/fixtures/ovis/complete_table.reference.txt').read_text()}])
    if 'zh_merged_table' in docs:
        refs = json.loads((ROOT / 'tests/fixtures/ovis/merged_table_book_page.manifest.json').read_text())
        report['samples']['zh_merged_table']['tables'] = table_quality(docs['zh_merged_table'], refs['tables'])
    if 'zh_formula' in docs:
        report['samples']['zh_formula']['formulas'] = formula_quality(docs['zh_formula'])
    if 'zh_pdf' in docs:
        report['samples']['zh_pdf']['text'] = pdf_quality(docs['zh_pdf'])
    if 'en_jee' in docs:
        report['samples']['en_jee']['anchors'] = anchors_quality(docs['en_jee'],
            ['JEE (Advanced) 2023', 'Q.12', 'Q.13', 'Time (h)'])
    if 'en_two_column' in docs:
        report['samples']['en_two_column']['anchors'] = anchors_quality(docs['en_two_column'], ANCHORS)
        page = docs['en_two_column']['pages'][0]
        actual_page_text = '\n'.join(b['provenance']['raw_output'] for b in page['blocks']
                                     if b['type'] == 'text')
        report['samples']['en_two_column']['whole_text_diagnostic'] = text_reference(
            actual_page_text, '\n'.join(ANCHORS))
        report['samples']['en_two_column']['image_blocks'] = [
            b['id'] for b in page['blocks'] if b['type'] == 'image']
        report['samples']['en_two_column']['caption_relations'] = [
            r for r in page['relations'] if r['type'] == 'caption_of']
    (output / 'quality.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({name: {'status': item.get('document_status'),
                             'non_ok': len(item.get('non_ok_blocks', []))}
                      for name, item in report['samples'].items()}, ensure_ascii=False))


if __name__ == '__main__':
    main()
