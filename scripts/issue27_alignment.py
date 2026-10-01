"""多 GT/单 Region 的保守字符对齐；原 #24 一对一口径独立保留。"""
import argparse
from collections import defaultdict
from copy import deepcopy
import json
from pathlib import Path
import re

from issue17_baseline import TEXT_CATEGORIES, formula, inline_formulas, normalize, overlap, summarize
from issue24_report import score
from issue24_run import save, sha

VERSION = 'issue27-content-alignment-1'
MATH = re.compile(r'(?<!\\)\$\$(.*?)\$\$|(?<!\\)\$(.*?)(?<!\\)\$|\\\((.*?)\\\)|\\\[(.*?)\\\]', re.S)


def math_payload(value):
    value = value.strip()
    for opening, closing in (('$$', '$$'), ('$', '$'), (r'\(', r'\)'), (r'\[', r'\]')):
        if value.startswith(opening) and value.endswith(closing) and len(value) >= len(opening) + len(closing):
            value = value[len(opening):-len(closing)]
            break
    return formula(value)


def score_inline(references, text_rows, indexed):
    parents = {r['annotation_id']: r for r in text_rows}
    used, rows = set(), []
    for ref in references:
        parent = parents[ref['anno_id']]
        block = indexed.get(parent['block_id'])
        raw = normalize(block['content'].get('text', '')) if block and parent['status'] == 'ok' else ''
        for span in inline_formulas(ref):
            occurrence = None
            for found in MATH.finditer(raw):
                key = (parent['block_id'], found.start(), found.end())
                bounds = parent['actual_range']
                latex = next(g for g in found.groups() if g is not None)
                if key not in used and bounds[0] <= found.start() < found.end() <= bounds[1] and \
                        math_payload(latex) == math_payload(span['latex']):
                    occurrence = key
                    used.add(key)
                    break
            rows.append(dict(parent_annotation_id=ref['anno_id'], block_id=parent['block_id'],
                status=parent['status'], exact=occurrence is not None, reference=span['latex'],
                actual_range=list(occurrence[1:]) if occurrence else None))
    return summarize(rows, [])


def score_order(supplementary, references, page):
    blocks = {b['id']: b for b in page.get('blocks', [])}
    refs = {r['anno_id']: r for r in references}
    positions = {bid: i for i, bid in enumerate(page.get('reading_order', []))}
    ordered = [(kind, row) for kind in ('text', 'independent_formula', 'table', 'figure')
               for row in supplementary[kind]['rows'] if isinstance(row.get('order'), int)]
    ordered.sort(key=lambda item: item[1]['order'])
    result = dict(denominator=len(ordered) * (len(ordered) - 1) // 2,
                  evaluable=0, correct=0, missing_pairs=0, unresolved_internal_pairs=[])
    for i, (ka, a) in enumerate(ordered):
        for kb, b in ordered[i + 1:]:
            if a['block_id'] not in positions or b['block_id'] not in positions:
                continue
            if a['block_id'] != b['block_id']:
                result['evaluable'] += 1
                result['correct'] += positions[a['block_id']] < positions[b['block_id']]
                continue
            # Never infer internal output order from GT order or the DP path.
            raw = normalize(blocks[a['block_id']]['content'].get('text', ''))
            wa, wb = (normalize(refs[r['annotation_id']].get('text', '')) for r in (a, b))
            if ka == kb == 'text' and wa and wb and a['status'] == b['status'] == 'ok' and \
                    raw.count(wa) == raw.count(wb) == 1:
                pa, pb = raw.find(wa), raw.find(wb)
                if pa + len(wa) <= pb or pb + len(wb) <= pa:
                    result['evaluable'] += 1
                    result['correct'] += pa < pb
                    continue
            result['unresolved_internal_pairs'].append([a['annotation_id'], b['annotation_id']])
    result['missing_pairs'] = result['denominator'] - result['evaluable']
    return result


def character_alignment(expected, actual):
    """最短编辑路径，记录每个输出字符唯一对应的参考位置。"""
    width = len(actual) + 1
    if (len(expected) + 1) * width > 25_000_000:
        raise ValueError('字符对齐超过有界内存预算，不能声称内容已核验')
    directions = [bytearray(width) for _ in range(len(expected) + 1)]
    directions[0][1:] = bytes([2]) * len(actual)
    previous = list(range(width))
    for i, left in enumerate(expected, 1):
        current = [i]
        directions[i][0] = 1
        for j, right in enumerate(actual, 1):
            costs = (previous[j - 1] + (left != right), previous[j] + 1, current[-1] + 1)
            direction = min(range(3), key=costs.__getitem__)
            current.append(costs[direction])
            directions[i][j] = direction
        previous = current
    operations = []
    i, j = len(expected), len(actual)
    while i or j:
        direction = directions[i][j]
        if direction == 0:
            i, j = i - 1, j - 1
            operations.append(('equal' if expected[i] == actual[j] else 'replace', i, j))
        elif direction == 1:
            i -= 1
            operations.append(('delete', i, j))
        else:
            j -= 1
            operations.append(('insert', i, j))
    return previous[-1], list(reversed(operations))


def attribute_operations(rows, operations, distance_key, range_key):
    owner = 0
    for kind, ri, ai in operations:
        while owner + 1 < len(rows) and ri >= rows[owner]['reference_range'][1]:
            owner += 1
        row = rows[owner]
        row[distance_key] += kind != 'equal'
        bounds = row[range_key]
        if bounds[0] is None:
            bounds[0] = ai
        bounds[1] = ai + (kind != 'delete')


def align_page(annotation, document):
    original = score(annotation, document)
    supplementary = deepcopy(original)
    page = document['pages'][0] if document.get('pages') else {}
    positions = {bid: i for i, bid in enumerate(page.get('reading_order', []))}
    blocks = [b for b in page.get('blocks', []) if b['type'] == 'text']
    references = [r for r in annotation['layout_dets']
                  if not r.get('ignore') and r['category_type'] in TEXT_CATEGORIES]
    groups, missing = defaultdict(list), []
    for ref in references:
        choices = sorted(((overlap(ref, b), b['id']) for b in blocks), reverse=True)
        if len(choices) > 1 and choices[0][0] >= .5 and abs(choices[0][0] - choices[1][0]) < 1e-9:
            missing.append((ref, 'ambiguous_geometry', sorted(bid for c, bid in choices if abs(c - choices[0][0]) < 1e-9)))
        elif choices and choices[0][0] >= .5:
            groups[choices[0][1]].append(ref)
        else:
            missing.append((ref, 'missing', []))
    rows, traces = [], []
    indexed = {b['id']: b for b in blocks}
    for bid, refs in groups.items():
        refs.sort(key=lambda r: (r.get('order') if isinstance(r.get('order'), int) else float('inf'),
                                 r['poly'][1], r['poly'][0], str(r['anno_id'])))
        block = indexed[bid]
        expected = ''.join(normalize(r.get('text', '')) for r in refs)
        usable = block['status'] == 'ok' and document.get('status') != 'failed'
        actual = normalize(block['content'].get('text', '')) if usable else ''
        distance, operations = character_alignment(expected, actual)
        raw = normalize(block['provenance'].get('raw_output', ''))
        raw_distance, raw_operations = character_alignment(expected, raw)
        offset, group_rows = 0, []
        for ref in refs:
            want = normalize(ref.get('text', ''))
            group_rows.append(dict(annotation_id=ref['anno_id'], category=ref['category_type'],
                order=ref.get('order'), block_id=bid, status=block['status'] if usable else 'failed_or_non_ok',
                reading_order_position=positions.get(bid),
                reference_chars=len(want), reference_range=[offset, offset + len(want)],
                actual_range=[None, None], raw_range=[None, None],
                edit_distance=0, raw_edit_distance=0, exact=False))
            offset += len(want)
        # Boundary insertions belong to the following GT; end insertions to the last.
        attribute_operations(group_rows, operations, 'edit_distance', 'actual_range')
        attribute_operations(group_rows, raw_operations, 'raw_edit_distance', 'raw_range')
        for row in group_rows:
            row['exact'] = bool(usable and row['reference_chars'] and row['edit_distance'] == 0)
            if row['actual_range'][0] is None:
                row['actual_range'] = [0, 0]
            if row['raw_range'][0] is None:
                row['raw_range'] = [0, 0]
        rows.extend(group_rows)
        traces.append(dict(block_id=bid, source_region_ids=block.get('source_region_ids', []),
            annotation_ids=[r['anno_id'] for r in refs], expected=expected, actual=actual,
            raw_output=block['provenance'].get('raw_output', ''), edit_distance=distance,
            raw_edit_distance=raw_distance, operations=operations, raw_operations=raw_operations,
            bbox=block['bbox'], assessment=block['provenance'].get('assessment'),
            source_layout_block_ids=[lid for region in page.get('regions', [])
                if region['id'] in block.get('source_region_ids', []) for lid in region['source_layout_block_ids']]))
    for ref, status, candidates in missing:
        want = normalize(ref.get('text', ''))
        rows.append(dict(annotation_id=ref['anno_id'], category=ref['category_type'], order=ref.get('order'),
            block_id=None, status=status, reading_order_position=None, reference_chars=len(want), edit_distance=len(want),
            raw_edit_distance=len(want), exact=False, actual_range=None, candidate_block_ids=candidates))
    extras = [dict(block_id=b['id'], reason='unmatched', status=b['status'],
                   insertion_chars=len(normalize(b['content'].get('text', '')))
                   if b['status'] == 'ok' and document.get('status') != 'failed' else 0)
              for b in blocks if b['id'] not in groups]
    supplementary['text'] = summarize(rows, extras)
    supplementary['text'].update(reference_chars=sum(r['reference_chars'] for r in rows),
        edit_distance=sum(r['edit_distance'] for r in rows) + sum(e['insertion_chars'] for e in extras),
        raw_edit_distance=sum(r['raw_edit_distance'] for r in rows))
    supplementary['inline_formula'] = score_inline(references, rows, indexed)
    supplementary['reading_order'] = score_order(supplementary, references, page)
    unresolved = [r['annotation_id'] for r in rows if not r['exact']]
    blockers = [f'{kind}:inexact_or_missing' for kind in ('inline_formula', 'independent_formula', 'table', 'figure')
                if supplementary[kind]['exact'] != supplementary[kind]['denominator']]
    blockers.extend(f'{kind}:extra_output' for kind in ('independent_formula', 'table', 'figure')
                    if supplementary[kind]['extra_outputs'])
    if supplementary['reading_order']['correct'] != supplementary['reading_order']['denominator']:
        blockers.append('reading_order:wrong_or_unverified')
    if not references:
        blockers.append('no_text_reference')
    if document.get('status') != 'ok':
        blockers.append('document_non_ok')
    verified = not unresolved and not extras and not blockers
    supplementary['quality_claim_blocked'] = not verified
    supplementary['quality_claim_blockers'] = blockers + \
        (['text:inexact_or_missing'] if unresolved else []) + (['text:extra_output'] if extras else [])
    refs_by_id = {r['anno_id']: r for r in annotation['layout_dets']}
    all_blocks = {b['id']: b for b in page.get('blocks', [])}
    non_text = []
    for kind in ('independent_formula', 'table', 'figure'):
        for row in supplementary[kind]['rows']:
            ref = refs_by_id[row['annotation_id']]
            block = all_blocks.get(row['block_id'])
            non_text.append(dict(kind=kind, annotation_id=row['annotation_id'], reference=ref,
                block_id=row['block_id'], source_region_ids=block.get('source_region_ids', []) if block else [],
                raw_output=block['provenance'].get('raw_output', '') if block else None,
                content=block['content'] if block else None, error=block.get('error') if block else None,
                status=row['status'], exact=row['exact']))
    return dict(version=VERSION, original=original, supplementary=supplementary,
        alignment=traces, non_text_evidence=non_text,
        reference_layout_dets=annotation['layout_dets'], content_audit=dict(verified=verified, blockers=blockers,
            unresolved_annotation_ids=unresolved, extra_text_block_ids=[e['block_id'] for e in extras],
            conclusion='逐字一致' if verified else '仍需核验'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--annotation', type=Path, required=True)
    parser.add_argument('--document', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    result = align_page(json.loads(args.annotation.read_text()), json.loads(args.document.read_text()))
    result['inputs_sha256'] = dict(annotation=sha(args.annotation), document=sha(args.document))
    save(args.out, result)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
