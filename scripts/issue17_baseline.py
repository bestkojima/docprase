"""运行固定 OmniDocBench 20 页，并按原始标注评分。"""
import argparse
from collections import Counter
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

from issue15_quality import ReferenceTable, edit_distance, normalize


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / 'docs/omnidocbench-20/manifest.json'
DATA = ROOT / 'output/omnidocbench/selected-20'
TEXT_CATEGORIES = {'text_block', 'title', 'header', 'footer', 'page_number',
                   'figure_caption', 'table_caption', 'table_footnote'}
KIND = {'table': 'table', 'equation_isolated': 'independent_formula', 'figure': 'figure'}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def box(poly):
    return [min(poly[::2]), min(poly[1::2]), max(poly[::2]), max(poly[1::2])]


def overlap(reference, actual):
    left, top, right, bottom = box(reference['poly'])
    x0, y0, x1, y1 = actual['bbox']
    area = max(0, right - left) * max(0, bottom - top)
    return (max(0, min(right, x1) - max(left, x0)) *
            max(0, min(bottom, y1) - max(top, y0)) / area if area else 0)


def inline_formulas(value):
    if isinstance(value, dict):
        if value.get('category_type') == 'equation_inline':
            yield value
            return
        for child in value.values():
            yield from inline_formulas(child)
    elif isinstance(value, list):
        for child in value:
            yield from inline_formulas(child)


def formula(value):
    value = value.strip()
    if value.startswith('$$') and value.endswith('$$'):
        value = value[2:-2]
    return re.sub(r'\s+', '', value)


def table_grid(html):
    # 上游表格有未转义的数学比较符；只在临时解析副本中转义。
    escaped = re.sub(r'<(?!/?(?:table|tr|td|th|br|thead|tbody|tfoot)\b)', '&lt;', html)
    parser = ReferenceTable()
    parser.feed(escaped)
    return parser.grid() if parser.cells else None


def repetition_signals(raw):
    compact = re.sub(r'\s+', '', raw)
    signals = []
    if len(compact) >= 48:
        pieces = Counter(compact[i:i + 16] for i in range(len(compact) - 15))
        if any(count >= 3 for count in pieces.values()):
            signals.append('repeated_16_character_fragment')
    if len(re.findall(r'(?m)^\s*#{1,3}\s*\d+\s*$', raw)) >= 5:
        signals.append('many_generated_number_headings')
    return signals


def summarize(rows, extras):
    denominator = len(rows)
    return {'denominator': denominator,
            'matched': sum(r['block_id'] is not None for r in rows),
            'missing': sum(r['block_id'] is None for r in rows),
            'fallback': sum(r['block_id'] is not None and r['status'] != 'ok' for r in rows),
            'exact': sum(r.get('exact') is True for r in rows),
            'duplicate_outputs': len([x for x in extras if x['reason'] == 'duplicate']),
            'unmatched_outputs': len([x for x in extras if x['reason'] == 'unmatched']),
            'rows': rows, 'extra_outputs': extras}


def match(reference, predictions, threshold):
    edges = sorted(((overlap(ref, pred), ri, pi)
                    for ri, ref in enumerate(reference)
                    for pi, pred in enumerate(predictions)), reverse=True)
    result = {}
    used = set()
    for coverage, ri, pi in edges:
        if coverage < threshold:
            break
        if ri not in result and pi not in used:
            result[ri] = (pi, coverage)
            used.add(pi)
    extras = []
    for pi, pred in enumerate(predictions):
        if pi not in used:
            highest = max((overlap(ref, pred) for ref in reference), default=0)
            extras.append({'block_id': pred['id'],
                           'reason': 'duplicate' if highest >= threshold else 'unmatched',
                           'status': pred['status']})
    return result, extras


def score_page(annotation, document):
    refs = [a for a in annotation['layout_dets'] if not a.get('ignore') and
            (a['category_type'] in TEXT_CATEGORIES or a['category_type'] in KIND)]
    page = document['pages'][0] if document.get('pages') else None
    blocks = page['blocks'] if page else []
    categories = {'text': [a for a in refs if a['category_type'] in TEXT_CATEGORIES],
                  'independent_formula': [a for a in refs if a['category_type'] == 'equation_isolated'],
                  'table': [a for a in refs if a['category_type'] == 'table'],
                  'figure': [a for a in refs if a['category_type'] == 'figure']}
    result = {}
    paired = {}
    for kind, expected in categories.items():
        output_type = {'independent_formula': 'formula', 'figure': 'image'}.get(kind, kind)
        predicted = [b for b in blocks if b['type'] == output_type]
        pairs, extras = match(expected, predicted, 0.5 if kind in ('text', 'table', 'figure') else 0.8)
        rows = []
        for ri, ref in enumerate(expected):
            pair = pairs.get(ri)
            pred = predicted[pair[0]] if pair else None
            row = {'annotation_id': ref['anno_id'], 'category': ref['category_type'],
                   'order': ref.get('order'), 'block_id': pred['id'] if pred else None,
                   'coverage': round(pair[1], 4) if pair else 0,
                   'status': pred['status'] if pred else 'missing'}
            actual = pred['content'].get('text', '') if pred and pred['status'] == 'ok' else ''
            if kind == 'text':
                want = normalize(ref.get('text', ''))
                got = normalize(actual)
                row.update({'reference_chars': len(want), 'edit_distance': edit_distance(got, want),
                            'exact': bool(pred and pred['status'] == 'ok' and got == want)})
            elif kind == 'independent_formula':
                row['exact'] = bool(pred and pred['status'] == 'ok' and
                                    formula(actual) == formula(ref['latex']))
            elif kind == 'table':
                reference_table = table_grid(ref['html'])
                actual_table = pred['content'].get('table') if pred and pred['status'] == 'ok' else None
                row['structure_exact'] = False
                row['cell_exact'] = 0
                row['cell_denominator'] = len(reference_table['cells']) if reference_table else 0
                if reference_table and actual_table:
                    fields = ('row', 'column', 'rowspan', 'colspan', 'header')
                    want = [tuple(c[f] for f in fields) for c in reference_table['cells']]
                    got = [tuple(c[f] for f in fields) for c in actual_table['cells']]
                    row['structure_exact'] = (reference_table['rows'], reference_table['columns'], want) == (
                        actual_table['rows'], actual_table['columns'], got)
                    row['cell_exact'] = sum(normalize(a.get('text', '')) == normalize(b['text'])
                                            for a, b in zip(actual_table['cells'], reference_table['cells']))
                row['exact'] = row['structure_exact'] and row['cell_exact'] == row['cell_denominator']
            else:
                row['exact'] = bool(pred and pred['status'] == 'ok')
            rows.append(row)
            if pred:
                paired[(kind, ri)] = pred
        result[kind] = summarize(rows, extras)

    inline_rows = []
    for ri, ref in enumerate(categories['text']):
        parent = paired.get(('text', ri))
        for span in inline_formulas(ref):
            want = formula(span['latex'])
            raw = parent['content'].get('text', '') if parent and parent['status'] == 'ok' else ''
            inline_rows.append({'parent_annotation_id': ref['anno_id'],
                                'block_id': parent['id'] if parent else None,
                                'status': parent['status'] if parent else 'missing',
                                'exact': bool(parent and parent['status'] == 'ok' and want in formula(raw))})
    result['inline_formula'] = summarize(inline_rows, [])
    ordered = [(row['order'], paired.get(('text', ri))) for ri, row in enumerate(result['text']['rows'])
               if isinstance(row['order'], int)]
    ordered += [(row['order'], paired.get(('independent_formula', ri)))
                for ri, row in enumerate(result['independent_formula']['rows']) if isinstance(row['order'], int)]
    ordered += [(row['order'], paired.get(('table', ri)))
                for ri, row in enumerate(result['table']['rows']) if isinstance(row['order'], int)]
    ordered += [(row['order'], paired.get(('figure', ri)))
                for ri, row in enumerate(result['figure']['rows']) if isinstance(row['order'], int)]
    expected = sorted(ordered, key=lambda item: item[0])
    positions = {bid: i for i, bid in enumerate(page['reading_order'])} if page else {}
    pairs_total = len(expected) * (len(expected) - 1) // 2
    observed = [(a, b) for i, a in enumerate(expected) for b in expected[i + 1:]
                if a[1] and b[1] and a[1]['id'] in positions and b[1]['id'] in positions]
    result['reading_order'] = {'denominator': pairs_total, 'evaluable': len(observed),
                               'correct': sum(positions[a[1]['id']] < positions[b[1]['id']]
                                              for a, b in observed),
                               'missing_pairs': pairs_total - len(observed)}
    result['block_statuses'] = dict(Counter(b['status'] for b in blocks))
    result['document_status'] = document.get('status')
    result['raw_repetition_suspicions'] = [
        {'block_id': b['id'], 'status': b['status'], 'signals': signals}
        for b in blocks if b['type'] == 'text'
        if (signals := repetition_signals(b.get('provenance', {}).get('raw_output', '')))]
    blockers = []
    if document.get('status') != 'ok':
        blockers.append('document_non_ok')
    for kind in ('text', 'inline_formula', 'independent_formula', 'table', 'figure'):
        metric = result[kind]
        for key in ('missing', 'fallback', 'duplicate_outputs', 'unmatched_outputs'):
            if metric[key]:
                blockers.append(f'{kind}:{key}')
        if metric['exact'] != metric['denominator']:
            blockers.append(f'{kind}:inexact')
    if result['reading_order']['correct'] != result['reading_order']['denominator']:
        blockers.append('reading_order_incomplete_or_wrong')
    if result['raw_repetition_suspicions']:
        blockers.append('raw_repetition_suspicions')
    result['quality_claim_blockers'] = blockers
    result['quality_claim_blocked'] = bool(blockers)
    return result


def verify_data(manifest, annotations):
    if len(manifest['pages']) != 20 or len(annotations) != 20:
        raise ValueError('固定清单与标注必须均为20页')
    if sha(DATA / 'OmniDocBench.json') != manifest['subset_annotation_sha256']:
        raise ValueError('子集标注哈希不符')
    for page, annotation in zip(manifest['pages'], annotations):
        source = DATA / page['image_path']
        if sha(source) != page['image_sha256'] or annotation['page_info'] != page['upstream_page_info']:
            raise ValueError(f"页面来源不符：{page['id']}")


def corrected_annotation(spec, annotation, errata):
    corrected = deepcopy(annotation)
    for entry in errata['items']:
        if entry['page_id'] != spec['id']:
            continue
        if entry['image_sha256'] != spec['image_sha256']:
            raise ValueError('勘误图像哈希不符')
        matches = [a for a in corrected['layout_dets'] if a.get('anno_id') == entry['annotation_id']]
        if len(matches) != 1 or matches[0].get('text', '').count(entry['upstream_fragment']) != 1:
            raise ValueError('勘误原文未唯一匹配')
        matches[0]['text'] = matches[0]['text'].replace(entry['upstream_fragment'],
                                                    entry['corrected_fragment'])
    return corrected


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cli', type=Path, default=ROOT / 'build/linux-current/dococr_cli')
    parser.add_argument('--out', type=Path, default=ROOT / 'output/issue-17')
    parser.add_argument('--score-only', action='store_true')
    parser.add_argument('--only', nargs='*', help='调试指定页面；正式基线不使用')
    args = parser.parse_args()
    manifest = json.loads(MANIFEST.read_text())
    annotations = json.loads((DATA / 'OmniDocBench.json').read_text())
    verify_data(manifest, annotations)
    errata_path = ROOT / 'docs/issue-17/errata.json'
    errata = json.loads(errata_path.read_text())
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=True)
    selected = set(args.only or [p['id'] for p in manifest['pages']])
    config = ROOT / 'configs/printed-page.example.json'
    report = {'dataset_revision': manifest['revision'], 'subset_sha256': sha(DATA / 'OmniDocBench.json'),
              'config_sha256': sha(config), 'cli_sha256': sha(args.cli.resolve()),
              'errata_sha256': sha(errata_path),
              'old_seven_manifest_sha256': sha(ROOT / 'docs/issue-15/samples.json'),
              'role': 'development_evaluation', 'pages': {}, 'aggregate': {}}
    for spec, annotation in zip(manifest['pages'], annotations):
        if spec['id'] not in selected:
            continue
        folder = output / spec['id']
        folder.mkdir(exist_ok=True)
        job = folder / 'job'
        command = [str(args.cli.resolve()), '--config', str(config), '--input',
                   str(DATA / spec['image_path']), '--out', str(job)]
        command_file = folder / 'command.json'
        if args.score_only or (job / 'document.json').is_file() or command_file.is_file():
            code = json.loads(command_file.read_text())['returncode'] if command_file.exists() else None
        else:
            try:
                with (folder / 'stdout.log').open('w') as stdout, (folder / 'stderr.log').open('w') as stderr:
                    completed = subprocess.run(['/usr/bin/time', '-v', '-o', str(folder / 'time.txt'),
                                                *command], cwd=ROOT, stdout=stdout, stderr=stderr,
                                               timeout=3600, check=False)
                code = completed.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout_3600s'
            command_file.write_text(json.dumps({'command': command, 'returncode': code},
                                                ensure_ascii=False, indent=2) + '\n')
        item = {'subject': spec['subject'], 'source_type': spec['upstream_page_info']['page_attribute']['data_source'],
                'source_document_identity': spec['source_document_identity'],
                'image_sha256': spec['image_sha256'], 'returncode': code}
        if (job / 'document.json').is_file():
            document = json.loads((job / 'document.json').read_text())
            item['scores'] = score_page(annotation, document)
            if spec['id'] == 'odb-15':
                revised = score_page(corrected_annotation(spec, annotation, errata), document)
                item['errata_text'] = revised['text']
            item['document_sha256'] = sha(job / 'document.json')
            if (job / 'run-manifest.json').exists():
                run = json.loads((job / 'run-manifest.json').read_text())
                item['run_manifest_sha256'] = sha(job / 'run-manifest.json')
                item['runtime'] = {k: run.get(k) for k in ('actual_backend', 'actual_device',
                                                            'effective_parameters', 'config_hash')}
                item['stop_reasons'] = dict(Counter(r.get('stop_reason') for r in run.get('regions', [])))
        else:
            item['failure'] = 'missing_document_output'
            stderr_path = folder / 'stderr.log'
            if stderr_path.exists():
                for line in stderr_path.read_text(errors='replace').splitlines():
                    try:
                        failure = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if isinstance(failure, dict) and failure.get('error'):
                        item['failure_detail'] = failure['error']
            item['scores'] = score_page(annotation, {'status': 'failed', 'pages': []})
            if spec['id'] == 'odb-15':
                revised = score_page(corrected_annotation(spec, annotation, errata),
                                     {'status': 'failed', 'pages': []})
                item['errata_text'] = revised['text']
        report['pages'][spec['id']] = item
        (output / 'progress.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
        print(spec['id'], code, item['scores']['document_status'], flush=True)
    report['aggregate']['pages_expected'] = 20
    report['aggregate']['pages_scored'] = len(report['pages'])
    report['aggregate']['pages_failed'] = sum(p['returncode'] != 0 or 'failure' in p
                                              for p in report['pages'].values()) + 20 - len(report['pages'])
    report['aggregate']['pages_non_ok'] = sum(p['scores']['document_status'] != 'ok'
                                             for p in report['pages'].values()) + 20 - len(report['pages'])
    report['aggregate']['quality_claim_blocked'] = (
        len(report['pages']) != 20 or any(p['scores']['quality_claim_blocked']
                                         for p in report['pages'].values()))
    report['aggregate']['raw_repetition_suspicions'] = sum(
        len(p['scores']['raw_repetition_suspicions']) for p in report['pages'].values())
    for kind in ('text', 'inline_formula', 'independent_formula', 'table', 'figure'):
        report['aggregate'][kind] = {key: sum(p['scores'][kind][key] for p in report['pages'].values())
                                     for key in ('denominator', 'matched', 'missing', 'fallback', 'exact',
                                                 'duplicate_outputs', 'unmatched_outputs')}
    report['aggregate']['text']['reference_chars'] = sum(
        row['reference_chars'] for page in report['pages'].values()
        for row in page['scores']['text']['rows'])
    report['aggregate']['text']['edit_distance'] = sum(
        row['edit_distance'] for page in report['pages'].values()
        for row in page['scores']['text']['rows'])
    report['aggregate']['text']['cer'] = (
        report['aggregate']['text']['edit_distance'] / report['aggregate']['text']['reference_chars']
        if report['aggregate']['text']['reference_chars'] else None)
    if 'odb-15' in report['pages']:
        original = report['pages']['odb-15']['scores']['text']
        corrected = report['pages']['odb-15']['errata_text']
        distance = report['aggregate']['text']['edit_distance'] - sum(
            row['edit_distance'] for row in original['rows']) + sum(
            row['edit_distance'] for row in corrected['rows'])
        chars = report['aggregate']['text']['reference_chars'] - sum(
            row['reference_chars'] for row in original['rows']) + sum(
            row['reference_chars'] for row in corrected['rows'])
        report['aggregate']['text_errata'] = {'reference_chars': chars, 'edit_distance': distance,
                                              'cer': distance / chars if chars else None,
                                              'exact': report['aggregate']['text']['exact'] - original['exact'] + corrected['exact']}
    report['aggregate']['table']['cells'] = sum(
        row['cell_denominator'] for page in report['pages'].values()
        for row in page['scores']['table']['rows'])
    report['aggregate']['table']['cells_exact'] = sum(
        row['cell_exact'] for page in report['pages'].values()
        for row in page['scores']['table']['rows'])
    report['aggregate']['reading_order'] = {key: sum(p['scores']['reading_order'][key]
                                                      for p in report['pages'].values())
                                            for key in ('denominator', 'evaluable', 'correct', 'missing_pairs')}
    (output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    return int(report['aggregate']['pages_failed'] > 0)


if __name__ == '__main__':
    sys.exit(main())
