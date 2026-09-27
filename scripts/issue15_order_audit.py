"""Audit the targeted reading-order fix against frozen real outputs."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

from issue15_acceptance import local_assets
from issue15_quality import ANCHORS, anchors_quality


ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def new_boundary_candidates(page):
    """List page geometry newly eligible under the title/header rule."""
    width = page['raster_size'][0]
    layouts = {b['id']: b for b in page['layout_blocks']}
    regions = {r['id']: r for r in page['regions']}
    blocks = page['blocks']
    labels = {}
    for block in blocks:
        layout_id = regions[block['source_region_ids'][0]]['source_layout_block_ids'][0]
        labels[block['id']] = layouts[layout_id]['model_label']
    left_edge = min((b['bbox'][0] for b in blocks if b['bbox'][0] < width * .45),
                    default=width)
    right = [b for b in blocks if b['bbox'][0] >= width * .52 and
             labels[b['id']] not in ('header', 'footer', 'footnote', 'vision_footnote')]
    new = []
    for b in blocks:
        label = labels[b['id']]
        x0, y0, x1, y1 = b['bbox']
        old = (x0 < width * .45 and x1 > width * .55 and
               (label in ('doc_title', 'paragraph_title') or x1-x0 >= width * .65))
        header = (label == 'header' and x0 < width * .45 and x1 > width * .55)
        title = (bool(right) and label in ('doc_title', 'paragraph_title') and
                 x0 <= left_edge + max(8, int(width * .03)) and
                 all(r['bbox'][3] < y0-(y1-y0) or r['bbox'][1] > y1+(y1-y0)
                     for r in right))
        if (header or title) and not old:
            new.append({'block_id': b['id'], 'model_label': label, 'bbox': b['bbox']})
    return new


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--initial', type=Path,
                        default=ROOT / 'output/issue-15/real-20260927')
    parser.add_argument('--fixed', type=Path,
                        default=ROOT / 'output/issue-15/reading-fix-20260927')
    parser.add_argument('--reexport', type=Path,
                        default=ROOT / 'output/issue-15/reading-fix-reexport')
    parser.add_argument('--out', type=Path,
                        default=ROOT / 'output/issue-15/order-audit.json')
    args = parser.parse_args()
    initial, fixed, copied = args.initial, args.fixed, args.reexport
    result = {'other_samples': {}, 'two_column': {}}
    for name in ('zh_text_table', 'zh_formula', 'zh_merged_table', 'en_jee', 'zh_pdf'):
        document = json.loads((initial / name / 'job/document.json').read_text())
        result['other_samples'][name] = [
            {'page_id': page['page_id'], 'new_boundary_candidates': new_boundary_candidates(page)}
            for page in document['pages']]
    before = json.loads((initial / 'en_two_column/job/document.json').read_text())
    after = json.loads((fixed / 'document.json').read_text())
    first = {b['id']: b for p in before['pages'] for b in p['blocks']}
    second = {b['id']: b for p in after['pages'] for b in p['blocks']}
    original_job = initial / 'en_two_column/job'
    assets = local_assets(before)
    asset_mismatches = [path for path in assets if sha(original_job / path) != sha(fixed / path)
                        or sha(fixed / path) != sha(copied / path)]
    result['two_column'] = {
        'new_boundary_candidates': new_boundary_candidates(before['pages'][0]),
        'blocks_equal_by_id': first == second,
        'first_order': before['pages'][0]['reading_order'],
        'fixed_order': after['pages'][0]['reading_order'],
        'first_anchor_score': {key: anchors_quality(before, ANCHORS)[key]
                               for key in ('pair_correct', 'pair_decidable', 'missing')},
        'fixed_anchor_score': {key: anchors_quality(after, ANCHORS)[key]
                               for key in ('pair_correct', 'pair_decidable', 'missing')},
        'assets_compared': len(assets), 'asset_mismatches': asset_mismatches,
        'fixed_json_reexport_equal': sha(fixed / 'document.json') == sha(copied / 'document.json'),
        'fixed_markdown_reexport_equal': sha(fixed / 'document.md') == sha(copied / 'document.md')}
    args.out.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    assert all(not p['new_boundary_candidates'] for pages in result['other_samples'].values()
               for p in pages)
    assert result['two_column']['blocks_equal_by_id']
    assert not asset_mismatches
    assert result['two_column']['fixed_anchor_score']['pair_correct'] == 105
    print(json.dumps({'other_pages': sum(map(len, result['other_samples'].values())),
                      'new_boundaries': len(result['two_column']['new_boundary_candidates']),
                      'asset_mismatches': len(asset_mismatches)}))


if __name__ == '__main__':
    sys.exit(main())
