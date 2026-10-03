"""验证人工裁决记录的完整性，生成索引与静态查看器；不裁决语义。"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import sys

CLASSES = {'strict_match', 'representation', 'transcription_error', 'coverage_error',
           'alignment', 'reference_issue', 'structure_error', 'export_fallback', 'unverified'}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')


def build(root):
    destination = root / 'docs/issue-31/content-review'
    sys.path.insert(0, str(root / 'scripts'))
    from issue17_baseline import table_grid
    frozen = json.loads((root / 'output/issue31-final-20261003/issue31-report.json').read_text())
    pages, kinds, classifications, cells, cell_classes, pending, hashes = [], Counter(), Counter(), 0, Counter(), [], {}
    for group, result in frozen['groups'].items():
        for key, frozen_page in result['pages'].items():
            name = f'{group}-{key}'
            file = destination / f'{name}.json'
            record = json.loads(file.read_text())
            assert record['page'] == name
            assert record['human_signed_off'] is False and record['scoring_unchanged'] is True
            assert record['all_content_verified'] is False
            assert record['frozen_candidate_sha256'] == frozen['candidate_sha256']
            assert record['frozen_evaluation_sha256'] == frozen['evaluation_sha256']
            assert sha(root / record['trace']['path']) == record['trace']['sha256']
            trace = json.loads((root / record['trace']['path']).read_text())
            assert record['source'] == trace['input']
            assert sha(Path(record['source']['path'])) == record['source']['sha256']
            blocks = {b['id']: b for b in trace['pages'][0]['blocks']}
            references = {str(r['anno_id']): r for r in frozen_page['reference_layout_dets']}
            aligned = {a['block_id']: a for a in frozen_page['alignment']}
            text_rows = {str(r['annotation_id']): r for r in frozen_page['parent']['text']['rows']}
            non_text = {e['kind'] + ':' + str(e['annotation_id']): e for e in frozen_page['non_text_evidence']}
            expected_ids = ['text:' + str(x['annotation_id']) for x in frozen_page['parent']['text']['rows']]
            expected_ids += [e['kind'] + ':' + str(e['annotation_id']) for e in frozen_page['non_text_evidence']
                             if e['kind'] in ('table', 'independent_formula')]
            for inline in frozen_page['inline_formula_diagnostics']:
                expected_ids.append(f"inline:{inline['parent_annotation_id']}:{len(expected_ids)}")
            expected_ids += ['extra:' + e['block_id'] for e in frozen_page['parent']['text']['extra_outputs']]
            assert expected_ids == [x['id'] for x in record['items']], name
            assert len(set(expected_ids)) == len(expected_ids)
            assert len(record['items']) == record['reviewed_units'] + len(record['unverified_units'])
            items_by_id = {i['id']: i for i in record['items']}
            for sheet, digest in record['viewed_sheets'].items():
                assert sha(root / 'output/issue31-visual-review-20261003' / name / sheet) == digest
            for detail, digest in record['detail_images'].items():
                assert sha(root / detail) == digest
            for item in record['items']:
                kinds[item['kind']] += 1
                review = item['review']
                assert set(review['classification']) <= CLASSES and review['classification'] and review['reason']
                assert review['reviewed'] == ('unverified' not in review['classification'])
                classifications.update(review['classification'])
                if not review['reviewed']:
                    pending.append(dict(page=name, id=item['id'], reason=review['reason']))
                if item['kind'] in ('text', 'table', 'independent_formula'):
                    reference = references[item['id'].split(':', 1)[1]]
                    assert item['reference'] == reference
                    assert item['expected'] == reference.get('text', reference.get('html', reference.get('latex')))
                if item['kind'] == 'text':
                    row = text_rows[item['id'].split(':', 1)[1]]
                    match, span = aligned.get(row['block_id']), row.get('actual_range')
                    actual = match['actual'][span[0]:span[1]] if match and span else ''
                    assert item['block_id'] == row['block_id'] and item['strict_exact'] == row['exact']
                    assert item['actual'] == actual
                    assert item['raw_parent'] == (match['raw_output'] if match else None)
                elif item['kind'] in ('table', 'independent_formula'):
                    evidence = non_text[item['id']]
                    assert item['block_id'] == evidence['block_id'] and item['strict_exact'] == evidence['exact']
                    assert item['actual'] == evidence.get('content', {})
                    assert item['raw_parent'] == evidence.get('raw_output')
                elif item['kind'] == 'inline':
                    diagnostic = frozen_page['inline_formula_diagnostics'][int(item['id'].rsplit(':', 1)[1]) -
                        len(text_rows) - sum(e['kind'] in ('table', 'independent_formula') for e in frozen_page['non_text_evidence'])]
                    assert item['row'] == diagnostic and item['expected'] == diagnostic['reference']
                    assert item['parent_item_id'] == 'text:' + str(diagnostic['parent_annotation_id'])
                    parent = items_by_id[item['parent_item_id']]
                    assert item['bbox'] == parent['bbox']
                    assert [b['id'] for b in item['evidence_blocks']] == [b['id'] for b in parent['evidence_blocks']]
                    if review['reviewed'] and 'coverage_error' not in review['classification']:
                        assert item['evidence_blocks'], (name, item['id'], '行内裁决缺父段落证据')
                elif item['kind'] == 'extra':
                    block = blocks[item['block_id']]
                    assert item['raw_parent'] == block['provenance']['raw_output']
                    assert item['actual'] == block['content'].get('text', '')
                evidence_ids = [b['id'] for b in item['evidence_blocks']]
                assert len(set(evidence_ids)) == len(evidence_ids)
                if item.get('block_id') is not None:
                    assert item['block_id'] in evidence_ids
                for evidence in item['evidence_blocks']:
                    block = blocks[evidence['id']]
                    assert evidence['raw_output'] == block['provenance']['raw_output']
                    assert evidence['final_content'] == block['content'] and evidence['bbox'] == block['bbox']
                    assert evidence['type'] == block['type'] and evidence['status'] == block['status']
                    assert evidence['error'] == block['error']
                if item['kind'] == 'table':
                    reference_cells = table_grid(item['expected'])['cells']
                    raw_cells = table_grid(item['raw_parent'])['cells']
                    final = item['actual'].get('table')
                    assert [c['reference'] for c in item['cell_reviews']] == reference_cells
                    for cell in item['cell_reviews']:
                        assert cell['reviewed'] and cell['reason'] and set(cell['classification']) <= CLASSES
                        assert all(c in raw_cells for c in cell['raw_model_cells'])
                        reference = cell['reference']
                        expected_final = [] if not final else [c for c in final['cells']
                            if (c['row'], c['column']) == (reference['row'], reference['column'])]
                        assert cell['final_cells'] == expected_final
                        cells += 1
                        cell_classes.update(cell['classification'])
                    assert item['final_export_review']['structured'] == bool(item['actual'].get('table'))
            hashes[file.name] = sha(file)
            pages.append(record)
    old_file = destination / 'old-seven-unverified.json'
    old = json.loads(old_file.read_text())
    assert {j['job'] for j in old['jobs']} == {p.stem for p in (root / 'docs/issue-31/evidence/pages').glob('old-seven*.json')}
    for job in old['jobs']:
        assert sha(root / job['trace']['path']) == job['trace']['sha256']
        trace = json.loads((root / job['trace']['path']).read_text())
        assert job['source'] == trace['input'] and sha(Path(job['source']['path'])) == job['source']['sha256']
        assert job['pages'] == len(trace['pages']) and job['previous_subset_quality'] == trace['quality']
        blocks = [(f"p{number:04}-{b['id']}", b) for number, page in enumerate(trace['pages'], 1)
                  for b in page['blocks'] if b['type'] in ('text', 'formula', 'table')]
        assert [i['id'] for i in job['items']] == [name for name, _ in blocks]
        for item, (_, block) in zip(job['items'], blocks):
            assert item['raw_output'] == block['provenance']['raw_output']
            assert item['final_content'] == block['content'] and item['bbox'] == block['bbox']
            assert item['kind'] == block['type']
        assert all(x['reviewed'] is False and x['classification'] == ['unverified'] for x in job['items'])
    assert kinds == dict(text=548, inline=263, independent_formula=16, table=16, extra=156)
    assert len(pages) == 32 and cells == 193 and len(pending) == 4
    assert sum(j['pages'] for j in old['jobs']) == 7
    hashes[old_file.name] = sha(old_file)
    summary = dict(review_method='Codex 逐项视觉与文本复核，非真人签署', human_signed_off=False,
        candidate_sha256=pages[0]['frozen_candidate_sha256'], evaluation_sha256=pages[0]['frozen_evaluation_sha256'],
        scoring_unchanged=True, complete_quality_acceptance=False, inspected_reference_pages=32,
        reference_units=843, additional_prediction_units=156, inspected_units=999, adjudicated_units=995,
        unverified=pending, counts_by_kind=dict(kinds), classification_unit_counts=dict(classifications),
        table_cells=193, cell_classification_counts=dict(cell_classes),
        count_note='标签可以重叠；正文、行内公式、拆出块与表格单元格也有重叠，不是独立错误次数，不用于重算 CER 或门槛。',
        old_seven=dict(pages=7,unverified_prediction_units=sum(len(j['items']) for j in old['jobs']),full_reference_available=False),
        validation=dict(reference_inventory=True, source_and_trace_sha256=True, viewed_sheet_sha256=True,
                        raw_and_final_evidence=True, table_cell_inventory=True),record_sha256=hashes)
    save(destination / 'summary.json', summary)
    data = json.dumps(dict(pages=pages, summary=summary), ensure_ascii=False).replace('<', '\\u003c')
    template = (destination / 'tools/viewer.html').read_text()
    (destination / 'review.html').write_text(template.replace('REVIEW_DATA_PLACEHOLDER', data))
    lines = ['# 内容差异裁决索引', '', '所有分类仅属于本轮视觉复核，未改变冻结评分。以下按项目列出，重复父项/子项不视为独立错误次数。', '']
    for category, title in [('transcription_error','真实转写错误'),('coverage_error','机器可读层覆盖缺失'),
                            ('structure_error','结构错误'),('export_fallback','最终导出降级'),('unverified','保留未核验项'),
                            ('reference_issue','参考标注差异'),('alignment','配对差异'),('representation','表示差异')]:
        lines += [f'## {title}', '']
        for page in pages:
            selected = [i for i in page['items'] if category in i['review']['classification']]
            if not selected:
                continue
            lines += [f"### [{page['page']}]({page['page']}.json)", '']
            for item in selected:
                lines += [f"- `{item['id']}`：{item['review']['reason']}"]
                if item['kind'] == 'table':
                    for cell in item['cell_reviews']:
                        if category in cell['classification']:
                            lines += [f"  - `{cell['id']}`：{cell['reason']}"]
            lines += ['']
    (destination / 'findings.md').write_text('\n'.join(lines).rstrip() + '\n')
    print(json.dumps(dict(pages=len(pages),units=sum(kinds.values()),cells=cells,pending=len(pending),integrity='passed'),ensure_ascii=False))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[4])
    args = parser.parse_args()
    build(args.root.resolve())
