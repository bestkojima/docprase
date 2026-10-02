"""#31 父区域评分：八项门槛；旧九项与行内公式诊断独立保留。"""
import argparse
from copy import deepcopy
import json
from pathlib import Path

from issue17_baseline import TEXT_CATEGORIES, box, overlap, table_grid, normalize
from issue24_report import aggregate, quality_metrics
from issue24_run import save, sha
from issue27_alignment import align_page, math_payload, MATH

VERSION = 'issue31-parent-region-1'
THRESHOLDS = {
    'text_cer': ('<=', .20),
    'independent_formula_exact_rate': ('>=', .80),
    'table_exact_rate': ('>=', .80),
    'table_cell_exact_rate': ('>=', .90),
    'reading_order_correct_rate': ('>=', .95),
    'fallback_rate': ('<=', .10),
    'missing_rate': ('<=', .05),
    'duplicate_outputs': ('<=', 0),
}


def reference_plan(annotation):
    """仅用原 GT 确定内嵌内容；预测的成功/失败不能改变参考分母。"""
    projected = deepcopy(annotation)
    refs = [r for r in projected['layout_dets'] if not r.get('ignore')]
    ownership, unresolved, children = [], [], set()
    for child in refs:
        category = child['category_type']
        if category not in TEXT_CATEGORIES | {'equation_isolated'}:
            continue
        parents = [r for r in refs if r is not child and
                   (r['category_type'] == 'table' or
                    (category == 'equation_isolated' and r['category_type'] in TEXT_CATEGORIES)) and
                   overlap(child, {'bbox': box(r['poly'])}) >= 1.0]
        if not parents:
            continue
        if len(parents) != 1:
            unresolved.append(dict(annotation_id=child['anno_id'], reason='ambiguous_reference_parent',
                                   candidates=[r['anno_id'] for r in parents]))
            continue
        parent = parents[0]
        if parent['category_type'] == 'table':
            grid = table_grid(parent.get('html', ''))
            texts = [c['text'] for c in grid['cells']] if grid else []
        else:
            texts = [parent.get('text', '')]
        expected = child.get('latex', '') if category == 'equation_isolated' else child.get('text', '')
        # Only collapse a redundant child reference whose content already occurs
        # exactly once in the original parent reference. Otherwise keep its score.
        normalizer = math_payload if category == 'equation_isolated' else normalize
        needle = normalizer(expected)
        occurrences = sum(normalizer(t).count(needle) for t in texts) if needle else 0
        if occurrences != 1:
            unresolved.append(dict(annotation_id=child['anno_id'], parent_annotation_id=parent['anno_id'],
                                   reason='child_content_not_unique_in_parent_reference'))
            continue
        children.add(child['anno_id'])
        ownership.append(dict(annotation_id=child['anno_id'], parent_annotation_id=parent['anno_id'],
                              parent_category=parent['category_type'], reference=deepcopy(child),
                              reason='content_already_counted_in_reference_parent'))
    projected['layout_dets'] = [r for r in projected['layout_dets'] if r['anno_id'] not in children]
    return projected, dict(owned_references=ownership, unresolved=unresolved,
                           original_reference_count=len(refs), parent_reference_count=len(refs)-len(children))


def formula_differences(aligned):
    """保留表示差异及完整父文本；不把未人工核验差异视为等价或正确。"""
    spans = {}
    for trace in aligned['alignment']:
        spans[trace['block_id']] = [dict(range=[m.start(), m.end()],
            latex=next(g for g in m.groups() if g is not None)) for m in MATH.finditer(trace['actual'])]
    return [dict(row, observed_parent_formulas=spans.get(row['block_id'], []),
                 classification='strict_match' if row['exact'] else 'unreviewed_content_or_representation_difference')
            for row in aligned['supplementary']['inline_formula']['rows']]


def evaluate_page(annotation, document):
    historical = align_page(annotation, document)
    projected, plan = reference_plan(annotation)
    aligned = align_page(projected, document) if plan['owned_references'] else historical
    current = deepcopy(aligned['supplementary'])
    blocks = {b['id']: b for p in document.get('pages', []) for b in p['blocks']}
    for kind in ('text', 'independent_formula'):
        for extra in current[kind]['extra_outputs']:
            if any(overlap(item['reference'], blocks[extra['block_id']]) >= .5
                   for item in plan['owned_references']):
                extra['reason'] = 'duplicate'
        current[kind]['duplicate_outputs'] = sum(e['reason'] == 'duplicate' for e in current[kind]['extra_outputs'])
        current[kind]['unmatched_outputs'] = sum(e['reason'] == 'unmatched' for e in current[kind]['extra_outputs'])
    # Inline exactness is diagnostic in #31. Text CER retains the unmodified
    # formula spelling; a differing formula cannot silently become verified.
    blockers = [b for b in current['quality_claim_blockers'] if not b.startswith('inline_formula:')]
    blockers.extend('reference_ownership:' + r['reason'] for r in plan['unresolved'])
    diagnostics = formula_differences(aligned)
    if any(not row['exact'] for row in diagnostics):
        blockers.append('inline_content_or_representation_difference_unreviewed')
    current['quality_claim_blockers'] = blockers
    current['quality_claim_blocked'] = bool(blockers)
    return dict(version=VERSION, original=historical['original'], parent=current,
                reference_plan=plan, alignment=aligned['alignment'],
                non_text_evidence=aligned['non_text_evidence'],
                inline_formula_diagnostics=diagnostics,
                content_audit=dict(verified=not blockers, blockers=blockers,
                    unresolved_annotation_ids=aligned['content_audit']['unresolved_annotation_ids']),
                reference_layout_dets=annotation['layout_dets'])


def totals(pages, variant='parent'):
    result = aggregate({k: dict(scores=p[variant], returncode=p.get('returncode', 0)) for k,p in pages.items()})
    if variant == 'parent':
        result['text']['edit_distance'] += sum(e.get('insertion_chars', 0)
            for p in pages.values() for e in p['parent']['text']['extra_outputs'])
    return result


def gates(value):
    metrics = quality_metrics(value)
    return {key: dict(value=metrics[key], operator=op, threshold=threshold,
        passed=metrics[key] is not None and
            (metrics[key] <= threshold if op == '<=' else metrics[key] >= threshold))
        for key,(op,threshold) in THRESHOLDS.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--annotation', type=Path, required=True)
    parser.add_argument('--document', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    result = evaluate_page(json.loads(args.annotation.read_text()), json.loads(args.document.read_text()))
    result.update(annotation_sha256=sha(args.annotation), document_sha256=sha(args.document),
                  scorer_sha256=sha(Path(__file__)), execution='saved_output_rescore_not_new_inference')
    result['metrics'] = gates(totals({'page': result}))
    result['quality_passed'] = all(m['passed'] for m in result['metrics'].values()) and result['content_audit']['verified']
    save(args.out, result)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
