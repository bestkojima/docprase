"""公共作业产物对照：定位、真实转写、内容归属及裁图覆盖分别评分。"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path

from issue17_baseline import DATA, MANIFEST, score_page, verify_data, inline_formulas, box
from issue20_compare import score, area, intersection, local_filter, recognition_units
import numpy as np


def ownership_audit(document, annotation=None):
    errors, truncated = [], []
    relations_count = 0
    isolated_conflicts, confirmed_inline, unmatched_formula = [], 0, 0
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
            if sum(child_id in r['source_layout_block_ids'] for r in owner_regions) != 1:
                errors.append(dict(type='child_missing_from_owner_region', relation=relation))
            a, b = child['bbox'], owner['bbox']
            if not (b[0] <= a[0] and b[1] <= a[1] and b[2] >= a[2] and b[3] >= a[3]):
                truncated.append(dict(child_id=child_id, owner_id=owner_id, child_bbox=a, owner_bbox=b))
            if annotation and child['label'] == 'formula':
                verified = False
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
                confirmed_inline += verified
                unmatched_formula += not verified
    return dict(relations=relations_count, errors=errors, owned_crop_truncations=truncated,
                isolated_formula_conflicts=isolated_conflicts, annotation_confirmed_inline=confirmed_inline,
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
            predictions = [dict(id=b['id'], bbox=b['bbox'], type=b['type'])
                           for b in document['pages'][0]['blocks']]
            run = json.loads(document_path.with_name('run-manifest.json').read_text())
            diagnostics = document['layout_diagnostics']
            raw = np.array([[c['class_id'], c['score'], *c['original_bbox'], c['rank']]
                            for c in diagnostics['candidates']], dtype='<f4')
            replay = local_filter(raw, tuple(document['pages'][0]['raster_size']), diagnostics['score_threshold'])
            expected = {c['candidate_id'] for c in diagnostics['candidates'] if c['selected']}
            actual = {c['id'] for c in replay}
            units, _ = recognition_units(replay)
            actual_units = {c['id'] for c in units}
            layouts = {l['id']: l for l in document['pages'][0]['layout_blocks']}
            regions = {r['id']: r for r in document['pages'][0]['regions']}
            expected_units = {layouts[regions[b['source_region_ids'][0]]['source_layout_block_ids'][0]]['candidate_id']
                              for b in document['pages'][0]['blocks']}
            row[name] = dict(document_sha256=hashlib.sha256(document_path.read_bytes()).hexdigest(),
                             layout=score(annotation, predictions), recognition=score_page(annotation, document),
                             ownership=ownership_audit(document, annotation), effective_parameters=run['effective_parameters'],
                             replay=dict(candidate_difference=sorted(actual ^ expected),
                                         recognition_unit_difference=sorted(actual_units ^ expected_units)))
        report['pages'][spec['id']] = row
    # 对照只纳入两侧均有产物的页面；完整运行必须覆盖20页。
    paired = {p: r for p, r in report['pages'].items() if 'before' in r and 'after' in r}
    report['paired_pages'] = list(paired)
    for name in ['before', 'after']:
        summary = dict(layout=Counter(), ownership=Counter(), recognition={})
        for row in paired.values():
            value = row[name]
            summary['layout'].update({k: v for k, v in value['layout']['total'].items() if k != 'matches'})
            summary['ownership'].update(relations=value['ownership']['relations'],
                                        errors=len(value['ownership']['errors']),
                                        owned_crop_truncations=len(value['ownership']['owned_crop_truncations']),
                                        isolated_formula_conflicts=len(value['ownership']['isolated_formula_conflicts']),
                                        annotation_confirmed_inline=value['ownership']['annotation_confirmed_inline'],
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
        report['aggregate'][name] = summary
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n')
    print('paired', len(paired))
    for side, values in report['aggregate'].items():
        print(side, json.dumps(values, ensure_ascii=False))


if __name__ == '__main__':
    main()
