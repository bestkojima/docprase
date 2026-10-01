"""按 #24 冻结门槛评估 #23 带标注全集；缺失页和回退保留完整分母。"""
import argparse
import json
import math
from pathlib import Path
import sys

from issue17_baseline import ROOT
from issue24_report import aggregate, audit, quality_metrics, score
from issue24_run import save, sha
from issue25_materials import (MANIFEST_PATH, THRESHOLDS_PATH, read_frozen, read_materials,
                               restore_page)


def missing_page(annotation, reason):
    return dict(returncode='not_run', scores=score(annotation, {'status': 'failed', 'pages': []}),
                errors=[reason], audit=dict(errors=[reason], non_ok=[], resources_sha256={}))


def evaluate_page(run, spec, annotation, record):
    try:
        with restore_page(run, spec['id']) as folder:
            receipt = json.loads((folder / 'command.json').read_text())
            errors = []
            bindings = dict(candidate_sha256=record['candidate_sha256'],
                evaluation_sha256=sha(run / 'evaluation.json'), input_sha256=spec['image_sha256'],
                annotation_sha256=spec['annotation_sha256'], execution='fresh_public_cli_job', models_intact=True)
            if any(receipt.get(key) != value for key, value in bindings.items()):
                errors.append('unbound_or_changed_execution_receipt')
            path = folder / 'job/document.json'
            document = json.loads(path.read_text()) if path.exists() else {'status': 'failed', 'pages': []}
            checked = audit(document, folder / 'job') if path.exists() else dict(
                errors=['missing_document'], non_ok=[], resources_sha256={})
            errors.extend(checked['errors'])
            code = receipt['returncode']
            if code != 0:
                errors.append('public_job_failed')
            scored = document if not errors else {'status': 'failed', 'pages': []}
            return dict(returncode=code if not errors else 'invalid_or_failed_result',
                scores=score(annotation, scored), errors=errors, command=receipt, audit=checked,
                document_sha256=sha(path) if path.exists() else None,
                document_status=document.get('status'), evidence=f"pages/{spec['id']}")
    except FileNotFoundError as error:
        return missing_page(annotation, f'missing_material_or_result:{error.filename}')


def threshold_results(totals, thresholds):
    metrics = quality_metrics(totals)
    rules = thresholds.get('metrics', {})
    if set(rules) != set(metrics):
        raise ValueError('缺少完整分项数值门槛')
    result = {}
    for key, rule in rules.items():
        limit = rule.get('threshold')
        operator = rule.get('operator')
        if (isinstance(limit, bool) or not isinstance(limit, (int, float)) or not math.isfinite(limit) or
                operator not in ('<=', '>=')):
            raise ValueError(f'非法或缺失数值门槛：{key}')
        value = metrics[key]
        passed = value is not None and (value <= limit if operator == '<=' else value >= limit)
        result[key] = dict(value=value, operator=operator, threshold=limit, passed=passed,
                          gap=None if value is None else max(0, value-limit if operator == '<=' else limit-value))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    run = args.run.resolve()
    frozen = (run / 'evaluation.json').exists()
    blockers = []
    if frozen:
        record, manifest, annotations = read_frozen(run)
        snapshot = run / 'freeze'
        candidate = json.loads((snapshot / 'docs/issue-24/evidence/candidate.json').read_text())
        if (sha(snapshot / 'docs/issue-24/evidence/candidate.json') != record['candidate_sha256'] or
                sha(snapshot / THRESHOLDS_PATH) != candidate['thresholds_sha256']):
            raise ValueError('冻结候选及门槛身份不一致')
        pages = {s['id']: evaluate_page(run, s, a, record) for s, a in zip(manifest['pages'], annotations)}
    else:
        snapshot = ROOT
        manifest = json.loads((ROOT / MANIFEST_PATH).read_text())
        manifest, annotations = read_materials(ROOT / MANIFEST_PATH, ROOT / manifest['subset_annotation']['path'])
        record = None
        pages = {s['id']: missing_page(a, 'evaluation_not_frozen_or_run')
                 for s, a in zip(manifest['pages'], annotations)}
        blockers.append('evaluation_not_frozen_or_run')
    thresholds = json.loads((snapshot / THRESHOLDS_PATH).read_text())
    totals = aggregate(pages)
    totals['pages_expected'] = len(manifest['pages'])
    totals['pages_non_ok'] = sum(p.get('document_status') != 'ok' for p in pages.values())
    results = threshold_results(totals, thresholds)
    engineering = json.loads((snapshot / 'docs/issue-24/report.json').read_text())
    engineering_gate = engineering['engineering_gate']
    if record and engineering['candidate_sha256'] != record['candidate_sha256']:
        raise ValueError('工程证据属于不同候选')
    if not engineering_gate['passed']:
        blockers.append('issue24_engineering_gate_not_passed')
    blockers.extend(f'{key}:threshold_not_met' for key, item in results.items() if not item['passed'])
    blockers.extend(f'{key}:{reason}' for key, page in pages.items() for reason in page['errors'])
    summary = json.loads((run / 'run-summary.json').read_text()) if (run / 'run-summary.json').exists() else {}
    before = json.loads((run / 'integrity-before.json').read_text()) if (run / 'integrity-before.json').exists() else {}
    evaluation_sha = sha(run / 'evaluation.json') if frozen else None
    if (summary.get('completed') != len(pages) or summary.get('expected') != len(pages) or
            not summary.get('candidate_and_materials_intact') or summary.get('failed') or
            not before.get('passed') or summary.get('evaluation_sha256') != evaluation_sha or
            before.get('evaluation_sha256') != evaluation_sha):
        blockers.append('incomplete_or_changed_run')
    verified_path = run / 'real-export-verification.json'
    verified = json.loads(verified_path.read_text()) if verified_path.exists() else {}
    if (not verified.get('passed') or verified.get('evaluation_sha256') != evaluation_sha or
            verified.get('pages_checked') != len(pages) or set(verified.get('records', {})) != set(pages) or
            any(verified['records'][key].get('document_sha256') != page.get('document_sha256')
                for key, page in pages.items())):
        blockers.append('real_schema_resources_and_reexport_not_verified')
    report = dict(role='annotated_evaluation_not_strict_independent_holdout', pages=pages,
        aggregate=totals, evaluation=record, evaluation_sha256=evaluation_sha,
        manifest_sha256=sha(snapshot / MANIFEST_PATH), dataset_revision=manifest['revision'],
        thresholds_sha256=sha(snapshot / THRESHOLDS_PATH), scorer_sha256=sha(Path(__file__)),
        engineering_gate=engineering_gate,
        integrity=dict(before=before, after=summary), export_verification=verified,
        annotation_disputes=dict(primary='unchanged_upstream', changed_annotations=0,
            known_new_errata=[], full_text_human_review=False,
            note='没有按本次输出修订参考；未做逐字人工校订，不宣称原始标注全部无误。'),
        content_audit=dict(missing='one_to_one_unmatched_GT_not_all_confirmed_content_loss',
            duplicate='spatial_same_category_extra_outputs_not_full_semantic_duplicate_audit',
            unresolved_granularity_and_structure='issue26_not_verified'),
        quality_gate=dict(metrics=results, numeric_targets_met=all(r['passed'] for r in results.values()),
            passed=not blockers, blockers=blockers,
            conclusion='本规格范围内质量达标' if not blockers else '本规格范围内产品质量未达标'))
    save(args.out, report)
    print(json.dumps(dict(quality_passed=not blockers,
        metrics={key: item['value'] for key, item in results.items()}), ensure_ascii=False))
    return 0 if not blockers else 2


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError) as error:
        print(f'评测输入或冻结证据非法：{error}', file=sys.stderr)
        raise SystemExit(1)
