"""公共作业产物对照：定位、真实转写、内容归属及裁图覆盖分别评分。"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path

from issue17_baseline import DATA, MANIFEST, ROOT, TEXT_CATEGORIES, score_page, verify_data, inline_formulas, box
from issue20_compare import score, area, intersection, local_filter, recognition_units
import numpy as np


def ownership_audit(document, annotation=None):
    errors, truncated = [], []
    relations_count = 0
    isolated_conflicts, confirmed_inline, unmatched_formula = [], 0, 0
    parent_confirmed, table_confirmed, parent_conflicts = 0, 0, []
    for page in document.get('pages', []):
        layouts = {l['id']: l for l in page['layout_blocks']}
        blocks = {b['id']: b for b in page['blocks']}
        regions = {r['id']: r for r in page['regions']}
        assignments = Counter(l for r in regions.values() for l in r['source_layout_block_ids'])
        for layout in layouts:
            if assignments[layout] != 1:
                errors.append(dict(type='layout_region_assignment', layout_id=layout, count=assignments[layout]))
        seen = set()
        for relation in page['relations']:
            if relation['type'] != 'content_owned_by':
                continue
            relations_count += 1
            child_id, owner_id = relation['source_layout_block_id'], relation['owner_block_id']
            if child_id in seen or child_id not in layouts or owner_id not in blocks:
                errors.append(dict(type='invalid_owner', relation=relation))
                continue
            seen.add(child_id)
            child, owner = layouts[child_id], blocks[owner_id]
            owner_regions = [regions[r] for r in owner['source_region_ids']]
            parent = layouts[owner_regions[0]['source_layout_block_ids'][0]]
            if sum(child_id in r['source_layout_block_ids'] for r in owner_regions) != 1:
                errors.append(dict(type='child_missing_from_owner_region', relation=relation))
            a, b = child['bbox'], owner['bbox']
            if not (b[0] <= a[0] and b[1] <= a[1] and b[2] >= a[2] and b[3] >= a[3]):
                truncated.append(dict(child_id=child_id, owner_id=owner_id, child_bbox=a, owner_bbox=b))
            if annotation and child['label'] == 'formula':
                verified = False
                matched_parents = set()
                for ref in annotation['layout_dets']:
                    if ref.get('ignore'):
                        continue
                    if ref['category_type'] == 'equation_isolated':
                        truth = box(ref['poly'])
                        inter = intersection(a, truth)
                        if inter / (area(a)+area(truth)-inter) >= .5:
                            isolated_conflicts.append(dict(child_id=child_id, owner_id=owner_id,
                                                           annotation_id=ref['anno_id']))
                    for span in inline_formulas(ref):
                        truth = box(span['poly'])
                        inter = intersection(a, truth)
                        if inter / (area(a)+area(truth)-inter) >= .3 and \
                            intersection(b, truth) / area(truth) >= .8:
                            verified = True
                            matched_parents.add(str(ref['anno_id']))
                confirmed_inline += verified
                unmatched_formula += not verified
                text_parents = []
                for ref in annotation['layout_dets']:
                    if ref.get('ignore') or ref['category_type'] not in TEXT_CATEGORIES:
                        continue
                    truth = box(ref['poly'])
                    inter = intersection(parent['bbox'], truth)
                    if inter/(area(parent['bbox'])+area(truth)-inter) >= .5:
                        text_parents.append(str(ref['anno_id']))
                parent_confirmed += len(matched_parents) == len(text_parents) == 1 and set(text_parents) == matched_parents
                if matched_parents and len(text_parents) == 1 and text_parents[0] not in matched_parents:
                    parent_conflicts.append(dict(child_id=child_id, owner_id=owner_id,
                                                 span_parent_annotations=sorted(matched_parents),
                                                 owner_text_annotation=text_parents[0]))
            if annotation and parent['label'] == 'table':
                for ref in annotation['layout_dets']:
                    if ref.get('ignore') or ref['category_type'] != 'table':
                        continue
                    truth = box(ref['poly'])
                    inter = intersection(parent['bbox'], truth)
                    if inter/(area(parent['bbox'])+area(truth)-inter) >= .5 and intersection(a, truth)/area(a) >= .8:
                        table_confirmed += 1
                        break
    return dict(relations=relations_count, errors=errors, owned_crop_truncations=truncated,
                isolated_formula_conflicts=isolated_conflicts, annotation_confirmed_inline=confirmed_inline,
                annotation_confirmed_inline_parent=parent_confirmed,
                annotation_confirmed_table_children=table_confirmed, annotation_parent_conflicts=parent_conflicts,
                unverified_formula_ownership=unmatched_formula)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--allow-partial', action='store_true')
    args = parser.parse_args()
    manifest = json.loads(MANIFEST.read_text())
    annotations = json.loads((DATA / 'OmniDocBench.json').read_text())
    verify_data(manifest, annotations)
    report = dict(role='development', dataset_sha256=manifest['subset_annotation_sha256'],
                  script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  recognition_scorer_sha256=hashlib.sha256((ROOT/'scripts/issue17_baseline.py').read_bytes()).hexdigest(),
                  geometry_scorer_sha256=hashlib.sha256((ROOT/'scripts/issue20_compare.py').read_bytes()).hexdigest(),
                  pages={}, aggregate={})
    for spec, annotation in zip(manifest['pages'], annotations):
        row = {}
        for name, directory in [('before', args.before), ('after', args.after)]:
            document_path = directory / spec['id'] / 'job/document.json'
            if not document_path.exists():
                if args.allow_partial:
                    continue
                raise ValueError(f'作业结果缺失：{document_path}')
            document = json.loads(document_path.read_text())
            page = document['pages'][0]
            layouts = {l['id']: l for l in page['layout_blocks']}
            regions = {r['id']: r for r in page['regions']}
            def primary_layout(block):
                return layouts[regions[block['source_region_ids'][0]]['source_layout_block_ids'][0]]
            # 与模型实验统一按父LayoutBlock评分；实际裁图和转写另行核验。
            predictions = [dict(id=b['id'], bbox=primary_layout(b)['bbox'], type=b['type']) for b in page['blocks']]
            run = json.loads(document_path.with_name('run-manifest.json').read_text())
            diagnostics = document['layout_diagnostics']
            raw = np.array([[c['class_id'], c['score'], *c['original_bbox'], c['rank']]
                            for c in diagnostics['candidates']], dtype='<f4')
            replay = local_filter(raw, tuple(document['pages'][0]['raster_size']), diagnostics['score_threshold'])
            expected = {c['candidate_id'] for c in diagnostics['candidates'] if c['selected']}
            actual = {c['id'] for c in replay}
            units, _ = recognition_units(replay)
            actual_units = {c['id'] for c in units}
            expected_units = {primary_layout(b)['candidate_id'] for b in page['blocks']}
            row[name] = dict(document_sha256=hashlib.sha256(document_path.read_bytes()).hexdigest(),
                             run_manifest_sha256=hashlib.sha256(document_path.with_name('run-manifest.json').read_bytes()).hexdigest(),
                             layout=score(annotation, predictions), recognition=score_page(annotation, document),
                             ownership=ownership_audit(document, annotation), effective_parameters=run['effective_parameters'],
                             replay=dict(candidate_difference=sorted(actual ^ expected),
                                         recognition_unit_difference=sorted(actual_units ^ expected_units)))
            receipt = document_path.parent.parent/'command.json'
            row[name]['execution_receipt'] = json.loads(receipt.read_text()) if receipt.exists() else None
        report['pages'][spec['id']] = row
    # 对照只纳入两侧均有产物的页面；完整运行必须覆盖20页。
    paired = {p: r for p, r in report['pages'].items() if 'before' in r and 'after' in r}
    report['paired_pages'] = list(paired)
    report['annotation_changes'] = dict(newly_matched=[], newly_unmatched=[])
    for page_id, row in paired.items():
        def matched(side):
            return {(c, str(a)) for c, m in row[side]['layout'].items() if c != 'total' for a, _ in m['matches']}
        before, after = matched('before'), matched('after')
        for key, values in [('newly_matched', after-before), ('newly_unmatched', before-after)]:
            report['annotation_changes'][key].extend(dict(page_id=page_id, category=c, annotation_id=a)
                                                     for c, a in sorted(values))
    for name in ['before', 'after']:
        summary = dict(layout=Counter(), ownership=Counter(), recognition={}, reading_order=Counter())
        for row in paired.values():
            value = row[name]
            summary['reading_order'].update(value['recognition']['reading_order'])
            summary['layout'].update({k: v for k, v in value['layout']['total'].items() if k != 'matches'})
            summary['ownership'].update(relations=value['ownership']['relations'],
                                        errors=len(value['ownership']['errors']),
                                        owned_crop_truncations=len(value['ownership']['owned_crop_truncations']),
                                        isolated_formula_conflicts=len(value['ownership']['isolated_formula_conflicts']),
                                        annotation_confirmed_inline=value['ownership']['annotation_confirmed_inline'],
                                        annotation_confirmed_inline_parent=value['ownership']['annotation_confirmed_inline_parent'],
                                        annotation_confirmed_table_children=value['ownership']['annotation_confirmed_table_children'],
                                        annotation_parent_conflicts=len(value['ownership']['annotation_parent_conflicts']),
                                        unverified_formula_ownership=value['ownership']['unverified_formula_ownership'])
            for category in ('text', 'inline_formula', 'independent_formula', 'table', 'figure'):
                metric = value['recognition'][category]
                summary['recognition'].setdefault(category, Counter()).update({k: metric[k] for k in
                    ('denominator', 'matched', 'missing', 'fallback', 'exact', 'duplicate_outputs', 'unmatched_outputs')})
            text = summary['recognition']['text']
            text.update(reference_chars=sum(r['reference_chars'] for r in value['recognition']['text']['rows']),
                        edit_distance=sum(r['edit_distance'] for r in value['recognition']['text']['rows']))
        m = summary['layout']
        m['f1'] = 2*m['matched']/(m['reference']+m['predicted']) if m['reference']+m['predicted'] else None
        text = summary['recognition'].get('text', {})
        text['cer'] = text.get('edit_distance', 0)/text['reference_chars'] if text.get('reference_chars') else None
        order = summary['reading_order']
        order['accuracy_on_evaluable_pairs'] = order['correct']/order['evaluable'] if order['evaluable'] else None
        report['aggregate'][name] = summary
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n')
    print('paired', len(paired))
    for side, values in report['aggregate'].items():
        print(side, json.dumps(values, ensure_ascii=False))


if __name__ == '__main__':
    main()
