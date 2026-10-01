"""从冻结的公共作业产物生成逐页回归对照及两关判定。"""
import argparse
from collections import Counter
from copy import deepcopy
import hashlib
import json
from pathlib import Path

from issue17_baseline import (DATA, MANIFEST, ROOT, corrected_annotation, edit_distance, normalize,
                              score_page, table_grid, verify_data)
from issue15_lineage import lineage_failures
from issue15_quality import (ANCHORS, anchors_quality, formula_quality, pdf_quality,
                             table_quality, text_reference)
from issue24_run import candidate_intact, save, sha

KINDS = ('text', 'inline_formula', 'independent_formula', 'table', 'figure')
TOP_KINDS = ('text', 'independent_formula', 'table', 'figure')
CONTENT_KINDS = ('text', 'independent_formula', 'table')
COUNTS = ('denominator', 'matched', 'missing', 'fallback', 'exact',
          'duplicate_outputs', 'unmatched_outputs')


def score(annotation, document):
    value = score_page(annotation, document)
    blocks = {b['id']: b for p in document.get('pages', []) for b in p['blocks']}
    positions = {bid: i for p in document.get('pages', []) for i, bid in enumerate(p['reading_order'])}
    for kind in TOP_KINDS:
        for row in value[kind]['rows']:
            row['reading_order_position'] = positions.get(row['block_id'])
    # 单元格必须同时具有正确位置、跨度及文字，不能只按数组下标计功。
    references = {str(a['anno_id']): a for a in annotation['layout_dets']}
    for row in value['text']['rows']:
        block = blocks.get(row['block_id'])
        raw = block['provenance'].get('raw_output', '') if block else ''
        expected = references[str(row['annotation_id'])].get('text', '')
        row['raw_edit_distance'] = edit_distance(normalize(raw), normalize(expected))
    fields = ('row', 'column', 'rowspan', 'colspan', 'header')
    for row in value['table']['rows']:
        reference = table_grid(references[str(row['annotation_id'])]['html'])
        block = blocks.get(row['block_id'])
        table = block['content'].get('table') if block and row['status'] == 'ok' else None
        cells = {tuple(c[k] for k in fields): normalize(c['text'])
                 for c in table['cells']} if table else {}
        row['cell_exact'] = sum(cells.get(tuple(c[k] for k in fields)) == normalize(c['text'])
                                for c in reference['cells']) if reference else 0
        row['exact'] = bool(row['status'] == 'ok' and row['structure_exact'] and
                            row['cell_exact'] == row['cell_denominator'])
    value['table']['exact'] = sum(row['exact'] for row in value['table']['rows'])
    return value


def aggregate(pages):
    scores = [p['scores'] for p in pages.values()]
    result = {kind: {key: sum(v[kind][key] for v in scores) for key in COUNTS} for kind in KINDS}
    result['text'].update(reference_chars=sum(r['reference_chars'] for v in scores for r in v['text']['rows']),
                          edit_distance=sum(r['edit_distance'] for v in scores for r in v['text']['rows']),
                          raw_edit_distance=sum(r['raw_edit_distance'] for v in scores for r in v['text']['rows']))
    chars = result['text']['reference_chars']
    result['text']['raw_transcription_cer_diagnostic'] = result['text']['raw_edit_distance'] / chars if chars else None
    result['table'].update(cells=sum(r['cell_denominator'] for v in scores for r in v['table']['rows']),
                           cells_exact=sum(r['cell_exact'] for v in scores for r in v['table']['rows']))
    result['reading_order'] = {key: sum(v['reading_order'][key] for v in scores)
                               for key in ('denominator', 'evaluable', 'correct', 'missing_pairs')}
    statuses = Counter()
    for value in scores:
        statuses.update(value['block_statuses'])
    result['block_statuses'] = statuses
    result['raw_repetition_suspicions'] = sum(len(v['raw_repetition_suspicions']) for v in scores)
    result['pages_scored'] = len(pages)
    result['pages_failed'] = sum(p['returncode'] != 0 for p in pages.values())
    return result


def quality_metrics(value):
    def ratio(numerator, denominator):
        return numerator / denominator if denominator else None
    content_total = sum(value[k]['denominator'] for k in CONTENT_KINDS)
    return dict(text_cer=ratio(value['text']['edit_distance'], value['text']['reference_chars']),
        inline_formula_exact_rate=ratio(value['inline_formula']['exact'], value['inline_formula']['denominator']),
        independent_formula_exact_rate=ratio(value['independent_formula']['exact'], value['independent_formula']['denominator']),
        table_exact_rate=ratio(value['table']['exact'], value['table']['denominator']),
        table_cell_exact_rate=ratio(value['table']['cells_exact'], value['table']['cells']),
        reading_order_correct_rate=ratio(value['reading_order']['correct'], value['reading_order']['denominator']),
        fallback_rate=ratio(sum(value[k]['fallback'] for k in CONTENT_KINDS), content_total),
        missing_rate=ratio(sum(value[k]['missing'] for k in CONTENT_KINDS), content_total),
        duplicate_outputs=sum(value[k]['duplicate_outputs'] for k in TOP_KINDS))


def compare(before, after):
    changes = {}
    for kind in TOP_KINDS:
        old = {str(r['annotation_id']): r for r in before[kind]['rows']}
        new = {str(r['annotation_id']): r for r in after[kind]['rows']}
        if old.keys() != new.keys():
            raise ValueError('前后标注分母不同，禁止比较')
        changes[kind] = dict(
            newly_missing=[key for key in old if old[key]['block_id'] and not new[key]['block_id']],
            newly_inexact=[key for key in old if old[key]['exact'] and not new[key]['exact']],
            recovered=[key for key in old if not old[key]['block_id'] and new[key]['block_id']],
            duplicate_delta=after[kind]['duplicate_outputs'] - before[kind]['duplicate_outputs'])
    old_inline, new_inline = before['inline_formula']['rows'], after['inline_formula']['rows']
    if len(old_inline) != len(new_inline) or any(
            a['parent_annotation_id'] != b['parent_annotation_id'] for a, b in zip(old_inline, new_inline)):
        raise ValueError('前后行内公式标注分母或顺序不同，禁止比较')
    # 源标注SHA固定，row_index是该冻结列表内稳定的公式身份。
    changes['inline_formula'] = dict(
        newly_missing=[dict(parent_annotation_id=a['parent_annotation_id'], row_index=i)
                       for i, (a, b) in enumerate(zip(old_inline, new_inline)) if a['block_id'] and not b['block_id']],
        newly_inexact=[dict(parent_annotation_id=a['parent_annotation_id'], row_index=i)
                       for i, (a, b) in enumerate(zip(old_inline, new_inline)) if a['exact'] and not b['exact']],
        recovered=[dict(parent_annotation_id=a['parent_annotation_id'], row_index=i)
                   for i, (a, b) in enumerate(zip(old_inline, new_inline)) if not a['block_id'] and b['block_id']],
        duplicate_delta=0)
    def indexed(value):
        return {(kind, str(r['annotation_id'])): r for kind in TOP_KINDS for r in value[kind]['rows']
                if isinstance(r.get('order'), int) and r['reading_order_position'] is not None}
    old, new = indexed(before), indexed(after)
    common = sorted(old.keys() & new.keys(), key=lambda k: (old[k]['order'], k))
    counts = Counter(evaluable=0, before_correct=0, after_correct=0, newly_wrong=0, corrected=0)
    wrong = []
    for i, a in enumerate(common):
        for b in common[i + 1:]:
            if old[a]['order'] == old[b]['order']:
                continue
            was = old[a]['reading_order_position'] < old[b]['reading_order_position']
            now = new[a]['reading_order_position'] < new[b]['reading_order_position']
            counts.update(evaluable=1, before_correct=int(was), after_correct=int(now),
                          newly_wrong=int(was and not now), corrected=int(not was and now))
            if was and not now:
                wrong.append([a, b])
    changes['common_order'] = dict(counts=counts, newly_wrong_pairs=wrong)
    before_chars = sum(r['reference_chars'] for r in before['text']['rows'])
    before_distance = sum(r['edit_distance'] for r in before['text']['rows'])
    after_distance = sum(r['edit_distance'] for r in after['text']['rows'])
    changes['text_cer_delta'] = (after_distance - before_distance) / before_chars if before_chars else None
    return changes


def audit(document, job):
    errors, non_ok, assets, attempts = [], [], {}, Counter()
    markdown = (job / 'document.md').read_text() if (job / 'document.md').exists() else ''
    for resource in document.get('resources', []):
        path = (job / resource['path']).resolve()
        if not path.is_relative_to(job.resolve()) or not path.is_file():
            errors.append(f"missing_or_outside_asset:{resource['path']}")
        else:
            assets[resource['path']] = sha(path)
    for page in document.get('pages', []):
        errors.extend(lineage_failures(page))
        layouts = {b['id']: b for b in page['layout_blocks']}
        blocks = {b['id']: b for b in page['blocks']}
        assigned = Counter(l for r in page['regions'] for l in r['source_layout_block_ids'])
        for key in layouts:
            if assigned[key] != 1:
                errors.append(f'{key}:region_assignment_count:{assigned[key]}')
        for relation in page['relations']:
            if relation['type'] == 'content_owned_by':
                child = layouts.get(relation['source_layout_block_id'])
                owner = blocks.get(relation['owner_block_id'])
                if not child or not owner:
                    errors.append('broken_ownership')
                elif not (owner['bbox'][0] <= child['bbox'][0] < child['bbox'][2] <= owner['bbox'][2]
                          and owner['bbox'][1] <= child['bbox'][1] < child['bbox'][3] <= owner['bbox'][3]):
                    errors.append(f"{child['id']}:owned_crop_truncated")
        if len(blocks) != len(page['blocks']) or set(page['reading_order']) != set(blocks) or len(page['reading_order']) != len(blocks):
            errors.append('invalid_reading_order_ids')
        for block in page['blocks']:
            if block['type'] not in ('text', 'formula', 'table'):
                continue
            provenance = block['provenance']
            visual = provenance.get('visual', {})
            recognition = provenance.get('recognition', {})
            if block['status'] == 'ok' and (visual.get('token_count', 0) <= 0 or
                                          provenance.get('assessment', {}).get('state') != 'ok'):
                errors.append(f"{block['id']}:silent_visual_or_assessment_failure")
            runs = recognition.get('attempts', [])
            attempts[len(runs)] += 1
            if len(runs) > 2 or not runs:
                errors.append(f"{block['id']}:invalid_attempt_count")
            for attempt in runs:
                configuration = attempt['config']
                if configuration['max_new_tokens'] > 4096 or configuration['generation_timeout_ms'] > 120000:
                    errors.append(f"{block['id']}:attempt_budget_exceeded")
                if (attempt.get('visual') or {}).get('token_count', 0) <= 0 and attempt['output'].get('raw_output'):
                    errors.append(f"{block['id']}:generation_without_visual_input")
            if block['status'] != 'ok':
                non_ok.append(dict(block_id=block['id'], status=block['status'], error=block.get('error'),
                    assessment=provenance.get('assessment'),
                    stop_reason=runs[-1]['output']['stop_reason'] if runs else None,
                    raw_sha256=hashlib.sha256(provenance.get('raw_output', '').encode()).hexdigest(),
                    attempts=len(runs)))
                resource = block['content'].get('resource')
                # JSON保留诊断文字；只检查不可靠文字是否出现在用户正文中。
                for value in (block['content'].get('text', ''), provenance.get('raw_output', '')):
                    normalized = normalize(value)
                    reliable_elsewhere = any(b['status'] == 'ok' and normalized in normalize(b['content'].get('text', ''))
                                             for b in page['blocks'])
                    if len(normalized) >= 16 and normalized in normalize(markdown) and not reliable_elsewhere:
                        errors.append(f"{block['id']}:unreliable_content_leaked")
                if not resource or f'![原图]({resource})' not in markdown:
                    errors.append(f"{block['id']}:fallback_not_visible")
    return dict(errors=errors, non_ok=non_ok, resources_sha256=assets, attempt_counts=attempts)


def read_after(folder, annotation, candidate_sha):
    receipt_path = folder / 'command.json'
    receipt = json.loads(receipt_path.read_text()) if receipt_path.exists() else {'returncode': 'not_run'}
    path = folder / 'job/document.json'
    document = json.loads(path.read_text()) if path.exists() else {'status': 'failed', 'pages': []}
    scored_document = document if receipt['returncode'] == 0 else {'status': 'failed', 'pages': []}
    errors = []
    if receipt.get('candidate_sha256') != candidate_sha or not receipt.get('models_intact'):
        errors.append('missing_or_changed_candidate_receipt')
    if receipt.get('execution') != 'fresh_public_cli_job':
        errors.append('not_fresh_execution')
    checked = audit(document, folder / 'job') if path.exists() else dict(errors=['missing_document'], non_ok=[], resources_sha256={})
    checked['errors'] += errors
    return dict(returncode=receipt['returncode'], scores=score(annotation, scored_document),
        document_sha256=sha(path) if path.exists() else None, audit=checked,
        evidence=str(folder.relative_to(ROOT)), command=receipt)


def old_quality(name, document, strict):
    """旧7页只有已有摘录/锚点参考，不冒充整页全文CER。"""
    document = deepcopy(document)
    if strict:
        for page in document['pages']:
            for block in page['blocks']:
                valid = block['status'] == 'ok' and document['status'] != 'failed'
                block['provenance']['raw_output'] = block['content'].get('text', '') if valid else ''
                if not valid:
                    block['content']['text'] = ''
                    block['content'].pop('table', None)
    if name == 'zh_pdf':
        return pdf_quality(document)
    if name == 'zh_formula':
        return formula_quality(document)
    if name in ('en_jee', 'en_two_column'):
        return anchors_quality(document, ANCHORS if name == 'en_two_column' else
                               ['JEE (Advanced) 2023', 'Q.12', 'Q.13', 'Time (h)'])
    if not document['pages']:
        return {'missing_document': True}
    if name == 'zh_merged_table':
        references = json.loads((ROOT / 'tests/fixtures/ovis/merged_table_book_page.manifest.json').read_text())['tables']
        return {'tables': table_quality(document, references)}
    manifest = json.loads((ROOT / 'tests/fixtures/ovis/manifest.json').read_text())
    ref = next(s for s in manifest['samples'] if s['name'] == 'complete_table')
    expected = (ROOT / 'tests/fixtures/ovis/chinese_text.reference.txt').read_text()
    candidates = [text_reference(b['provenance']['raw_output'], expected)
                  for p in document['pages'] for b in p['blocks'] if b['type'] == 'text']
    return dict(text_excerpt=min(candidates, key=lambda r: r['edit_distance']) if candidates else text_reference('', expected),
                tables=table_quality(document, [dict(annotation_id=ref['annotation_id'], polygon=ref['source_polygon'],
                      html=(ROOT / 'tests/fixtures/ovis/complete_table.reference.txt').read_text())]))


def old_seven(run, candidate_sha):
    result, blockers = {}, []
    manifest = json.loads((ROOT / 'docs/issue-15/samples.json').read_text())
    for spec in manifest['samples']:
        name = spec['id']
        baseline_folder = 'reading-fix' if name == 'en_two_column' else name
        before_path = ROOT / 'docs/issue-15/evidence' / baseline_folder / 'document.json'
        after_path = run / 'old-seven' / name / 'job/document.json'
        old = json.loads(before_path.read_text())
        new = json.loads(after_path.read_text()) if after_path.exists() else {'status': 'failed', 'pages': []}
        receipt_path = after_path.parent.parent / 'command.json'
        receipt = json.loads(receipt_path.read_text()) if receipt_path.exists() else {}
        checked = audit(new, after_path.parent)
        if receipt.get('returncode') != 0 or receipt.get('candidate_sha256') != candidate_sha or not receipt.get('models_intact'):
            blockers.append(f'{name}:missing_or_failed_or_changed_candidate')
        if len(new['pages']) != spec['pages']:
            blockers.append(f'{name}:missing_page')
        before, after = old_quality(name, old, True), old_quality(name, new, True)
        if name == 'zh_pdf' and after['total_edit_distance'] > before['total_edit_distance']:
            blockers.append(f'{name}:strict_pdf_text_regression')
        if name in ('en_jee', 'en_two_column'):
            lost = set(after['missing']) - set(before['missing'])
            old_wrong = {tuple(p) for p in before['wrong_pairs']}
            new_wrong = {tuple(p) for p in after['wrong_pairs']}
            if lost or new_wrong - old_wrong:
                blockers.append(f'{name}:lost_anchor_or_new_order_error')
        if name == 'zh_text_table' and after.get('text_excerpt', {}).get('edit_distance', 0) > before['text_excerpt']['edit_distance']:
            blockers.append(f'{name}:strict_excerpt_regression')
        if name == 'zh_formula':
            for old_row, new_row in zip(before['independent'], after['independent']):
                if old_row.get('exact_ignoring_whitespace') and not new_row.get('exact_ignoring_whitespace'):
                    blockers.append(f'{name}:independent_formula_regression')
        for old_row, new_row in zip(before.get('tables', []), after.get('tables', [])):
            if old_row.get('structure_exact') and not new_row.get('structure_exact') or new_row.get('cell_exact', 0) < old_row.get('cell_exact', 0):
                blockers.append(f'{name}:table_regression')
        blockers += [f'{name}:{error}' for error in checked['errors']]
        result[name] = dict(pages_expected=spec['pages'], pages_observed=len(new['pages']),
            input_sha256=spec['sha256'], before_document_sha256=sha(before_path),
            after_document_sha256=sha(after_path) if after_path.exists() else None,
            document_status=new['status'], strict_before=before, strict_after=after,
            raw_diagnostic_after=old_quality(name, new, False), audit=checked, command=receipt)
    known = []
    examples = [('en_jee', [55, 26, 137, 35], 'JEE (Advanced) 2023'),
                ('en_two_column', [714, 658, 1038, 691], 'Figure 1. Recorded readings.')]
    for name, bbox, expected in examples:
        path = run / 'old-seven' / name / 'job/document.json'
        doc = json.loads(path.read_text()) if path.exists() else {'pages': []}
        x0, y0, x1, y1 = bbox
        found = [b for p in doc['pages'] for b in p['blocks'] if b['type'] == 'text' and
                 max(0, min(x1, b['bbox'][2])-max(x0, b['bbox'][0])) *
                 max(0, min(y1, b['bbox'][3])-max(y0, b['bbox'][1])) / ((x1-x0)*(y1-y0)) >= .5]
        restored = any(b['status'] == 'ok' and expected in b['content']['text'] and
                       b['provenance'].get('visual', {}).get('token_count', 0) > 0 for b in found)
        explicit_failure = bool(found) and all(b['status'] != 'ok' for b in found)
        known.append(dict(sample=name, historical_bbox=bbox, expected=expected,
                          restored=restored, explicit_failure=explicit_failure, block_ids=[b['id'] for b in found]))
        if not restored and not explicit_failure:
            blockers.append(f'{name}:known_small_crop_neither_recovered_nor_explicit_failure')
    return dict(samples=result, known_small_crops=known, blockers=blockers)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', type=Path, required=True)
    parser.add_argument('--out', type=Path, default=ROOT / 'docs/issue-24/report.json')
    args = parser.parse_args()
    run = args.run.resolve()
    manifest = json.loads(MANIFEST.read_text())
    annotations = json.loads((DATA / 'OmniDocBench.json').read_text())
    verify_data(manifest, annotations)
    frozen = json.loads((run / 'candidate.json').read_text())
    if sha(MANIFEST) != frozen['dataset_manifest_sha256'] or sha(ROOT / 'docs/issue-15/samples.json') != frozen['old_manifest_sha256']:
        raise ValueError('运行后样本清单变化，禁止重新定义分母')
    if not candidate_intact(run, frozen):
        raise ValueError('模型、运行时、配置或门槛不再符合冻结哈希')
    thresholds_path = run / 'candidate/thresholds.json'
    if sha(thresholds_path) != frozen['thresholds_sha256']:
        raise ValueError('冻结门槛哈希不符')
    thresholds = json.loads(thresholds_path.read_text())
    baseline = json.loads((ROOT / 'docs/issue-17/report.json').read_text())
    if sha(ROOT / 'docs/issue-17/report.json') != thresholds['basis']['sha256']:
        raise ValueError('修复前基线哈希不符')
    errata = json.loads((ROOT / 'docs/issue-17/errata.json').read_text())
    report = dict(role='development_regression_not_final_quality_acceptance', candidate=frozen,
        candidate_sha256=sha(run / 'candidate.json'), thresholds_sha256=sha(thresholds_path),
        scorer_sha256=sha(Path(__file__)), legacy_scorer_sha256=sha(ROOT / 'scripts/issue17_baseline.py'),
        dataset_revision=manifest['revision'], dataset_sha256=manifest['subset_annotation_sha256'],
        table_cell_policy='position_span_header_and_text', pages={}, aggregate={}, regression_blockers=[])
    for spec, annotation in zip(manifest['pages'], annotations):
        old = baseline['pages'][spec['id']]
        old_path = ROOT / f"docs/issue-17/evidence/{spec['id']}/job/document.json"
        if old_path.exists():
            if sha(old_path) != old['document_sha256']:
                raise ValueError(f'历史产物哈希不符：{old_path}')
            document = json.loads(old_path.read_text())
        else:
            if old['returncode'] == 0:
                raise ValueError(f'历史成功页产物缺失：{old_path}')
            document = {'status': 'failed', 'pages': []}
        before = dict(returncode=old['returncode'], scores=score(annotation, document),
                      document_sha256=old.get('document_sha256'), execution='historical_baseline')
        after = read_after(run / 'development' / spec['id'], annotation, report['candidate_sha256'])
        delta = compare(before['scores'], after['scores'])
        report['pages'][spec['id']] = dict(subject=spec['subject'], image_sha256=spec['image_sha256'],
                                          before=before, after=after, comparison=delta)
        for kind in KINDS:
            if delta[kind]['newly_missing'] or delta[kind]['newly_inexact'] or delta[kind]['duplicate_delta'] > 0:
                report['regression_blockers'].append(f"{spec['id']}:{kind}:lost_match_or_quality_or_duplicate")
        if delta['common_order']['counts']['newly_wrong'] or (delta['text_cer_delta'] or 0) > 0:
            report['regression_blockers'].append(f"{spec['id']}:order_or_cer_regression")
        report['regression_blockers'] += [f"{spec['id']}:{e}" for e in after['audit']['errors']]
        if after['returncode'] != 0:
            report['regression_blockers'].append(f"{spec['id']}:run_failed")
        if spec['id'] == 'odb-15':
            corrected = corrected_annotation(spec, annotation, errata)
            new_path = run / 'development' / spec['id'] / 'job/document.json'
            new_document = json.loads(new_path.read_text()) if new_path.exists() and after['returncode'] == 0 else {'status': 'failed', 'pages': []}
            report['annotation_dispute'] = dict(errata=errata, primary='unchanged_upstream',
                before_corrected=score(corrected, document)['text'], after_corrected=score(corrected, new_document)['text'])
    for side in ('before', 'after'):
        report['aggregate'][side] = aggregate({key: row[side] for key, row in report['pages'].items()})
    report['old_seven'] = old_seven(run, report['candidate_sha256'])
    report['regression_blockers'] += report['old_seven']['blockers']
    metrics = quality_metrics(report['aggregate']['after'])
    results = {key: dict(value=metrics[key], **rule,
                         passed=metrics[key] is not None and
                         (metrics[key] <= rule['threshold'] if rule['operator'] == '<=' else metrics[key] >= rule['threshold']))
               for key, rule in thresholds['metrics'].items()}
    conditions_path = ROOT / 'docs/issue-24/engineering-conditions.json'
    conditions = json.loads(conditions_path.read_text())
    if conditions.get('candidate_sha256') != report['candidate_sha256']:
        for item in conditions['conditions']:
            if item['status'] == 'passed':
                item['status'] = 'not_verified_for_this_candidate'
    for item in conditions['conditions']:
        if item['status'] == 'passed':
            evidence = item.get('evidence', {})
            if not evidence or any(sha(ROOT / path) != digest for path, digest in evidence.items()):
                raise ValueError(f"工程验收依据缺失或哈希不符：{item['id']}")
    blockers = list(report['regression_blockers'])
    blockers += [item['id'] for item in conditions['conditions'] if item['status'] != 'passed']
    summary_path = run / 'run-summary.json'
    summary = json.loads(summary_path.read_text()) if summary_path.exists() else {}
    if summary.get('completed') != 26 or not summary.get('candidate_intact') or summary.get('failed'):
        blockers.append('incomplete_or_changed_20_plus_7_run')
    report['engineering_gate'] = dict(passed=not blockers, blockers=blockers, conditions=conditions,
                                      conditions_sha256=sha(conditions_path))
    report['quality_gate'] = dict(numeric_targets_on_development=results,
        development_targets_met=all(item['passed'] for item in results.values()),
        passed=False, reason='separate_annotated_evaluation_not_run',
        conclusion='独立质量验收尚未完成；开发集成绩不构成最终质量通过')
    save(args.out, report)
    print(json.dumps(dict(engineering_passed=not blockers,
                          development_metrics=metrics, quality_passed=False), ensure_ascii=False))
    return 0 if not blockers else 2


if __name__ == '__main__':
    raise SystemExit(main())
