"""原口径与补充对齐分别评分；未跑/不完整/未核验均不能通过。"""
import argparse
import json
from pathlib import Path
import sys

from issue17_baseline import ROOT
from issue24_report import aggregate, audit, compare, old_seven
from issue24_run import save, sha
from issue25_materials import restore_page, THRESHOLDS_PATH
from issue25_report import threshold_results
from issue27_alignment import align_page, VERSION
from issue27_evidence import check_frozen, read, suite_specs


def totals(pages, variant):
    values = {key: dict(scores=page[variant], returncode=page['returncode']) for key, page in pages.items()}
    result = aggregate(values)
    if variant == 'supplementary':
        result['text']['edit_distance'] += sum(e.get('insertion_chars', 0)
            for p in pages.values() for e in p[variant]['text']['extra_outputs'])
    return result


def historical_pages(jobs, baseline):
    pages = {}
    for job in jobs:
        if not job['key'].startswith('evaluation/'):
            continue
        page_id = job['spec']['id']
        with restore_page(baseline, page_id) as folder:
            document = read(folder / 'job/document.json')
            value = align_page(job['annotation'], document)
            value.update(returncode=read(folder / 'command.json')['returncode'],
                document_sha256=sha(folder / 'job/document.json'),
                evidence=f'output/issue-25/pages/{page_id}', input_sha256=job['input_sha256'])
            pages[page_id] = value
    return pages


def development_baseline(jobs):
    pages = {}
    for job in jobs:
        if not job['key'].startswith('development/'):
            continue
        path = ROOT / 'docs/issue-24/evidence' / job['key'] / 'job/document.json'
        value = align_page(job['annotation'], read(path))
        value.update(returncode=0, document_sha256=sha(path), evidence=str(path), input_sha256=job['input_sha256'])
        pages[job['spec']['id']] = value
    return pages


def evaluate_job(run, job, record):
    folder = run / job['key']
    path = folder / 'job/document.json'
    errors = []
    receipt = read(folder / 'command.json') if (folder / 'command.json').exists() else {}
    bindings = dict(candidate_sha256=record['candidate_sha256'], evaluation_sha256=sha(run / 'evaluation.json'),
                    input_sha256=job['input_sha256'], execution='fresh_public_cli_job', models_intact=True,
                    returncode=0)
    if any(receipt.get(key) != value for key, value in bindings.items()):
        errors.append('missing_or_failed_or_unbound_execution')
    try:
        for name, digest in receipt.get('files_sha256', {}).items():
            source = (folder / name).resolve()
            if not source.is_relative_to(folder.resolve()) or sha(source) != digest:
                errors.append(f'changed_artifact:{name}')
        document = read(path)
        checked = audit(document, folder / 'job')
        errors.extend(checked['errors'])
    except (OSError, ValueError) as error:
        document = dict(status='failed', pages=[])
        checked = dict(errors=[str(error)])
        errors.append('missing_or_invalid_document')
    aligned = align_page(job['annotation'], document if not errors else dict(status='failed', pages=[]))
    aligned.update(returncode=receipt.get('returncode', 'not_run') if not errors else 'invalid_or_failed_result',
        errors=errors, audit=checked, document_sha256=sha(path) if path.exists() else None,
        input_sha256=job['input_sha256'], evidence=str(folder), command=receipt)
    return aligned


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', type=Path, required=True)
    parser.add_argument('--baseline', type=Path, default=ROOT / 'output/issue-25')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--baseline-only', action='store_true')
    args = parser.parse_args()
    run = args.run.resolve()
    blockers = []
    record = None
    if (run / 'evaluation.json').exists() and not args.baseline_only:
        record, *_ = check_frozen(run, Path(__file__))
        jobs = record['jobs']
        thresholds = read(run / 'freeze' / THRESHOLDS_PATH)
    else:
        jobs, _ = suite_specs()
        thresholds = read(ROOT / THRESHOLDS_PATH)
        blockers.append('evaluation_not_frozen_or_not_executed')
    baseline = historical_pages(jobs, args.baseline.resolve())
    development_before = development_baseline(jobs)
    groups = {}
    if not args.baseline_only:
        for group in ('development', 'evaluation'):
            pages = {}
            for job in jobs:
                if not job['key'].startswith(group + '/'):
                    continue
                if record:
                    value = evaluate_job(run, job, record)
                else:
                    value = align_page(job['annotation'], dict(status='failed', pages=[]))
                    value.update(returncode='not_run', errors=['not_run'])
                pages[job['spec']['id']] = value
            variants = {variant: totals(pages, variant) for variant in ('original', 'supplementary')}
            groups[group] = dict(pages=pages, aggregate=variants,
                metrics={variant: threshold_results(v, thresholds) for variant, v in variants.items()})
        blockers.extend(f'evaluation:{key}:original_threshold_not_met'
            for key, metric in groups['evaluation']['metrics']['original'].items() if not metric['passed'])
        blockers.extend(f'{group}:{key}:{error}' for group, result in groups.items()
            for key, page in result['pages'].items() for error in page.get('errors', []))
        blockers.extend(f'{group}:{key}:content_still_unverified' for group, result in groups.items()
            for key, page in result['pages'].items() if not page['content_audit']['verified'])
    summary = read(run / 'run-summary.json') if (run / 'run-summary.json').exists() else {}
    export = read(run / 'real-export-verification.json') if (run / 'real-export-verification.json').exists() else {}
    automated = read(run / 'automated-verification.json') if (run / 'automated-verification.json').exists() else {}
    safety = read(run / 'safety/summary.json') if (run / 'safety/summary.json').exists() else {}
    safety_binding = read(run / 'safety-verification.json') if (run / 'safety-verification.json').exists() else {}
    old = old_seven(run, record['candidate_sha256']) if record and summary.get('completed') == 38 else None
    if not record or summary.get('completed') != 38 or summary.get('expected') != 38 or summary.get('failed') or \
            summary.get('evaluation_sha256') != sha(run / 'evaluation.json') or \
            not summary.get('candidate_and_materials_intact'):
        blockers.append('full_39_pages_new_inference_not_verified')
    if not record or not export.get('passed') or export.get('pages_checked') != 39 or \
            export.get('evaluation_sha256') != sha(run / 'evaluation.json'):
        blockers.append('schema_resources_and_reexport_not_verified')
    if not automated.get('passed') or not record or automated.get('candidate_sha256') != record['candidate_sha256']:
        blockers.append('automated_regression_not_verified_for_candidate')
    if not record or safety_binding.get('candidate_sha256') != record['candidate_sha256'] or \
            safety_binding.get('evaluation_sha256') != sha(run / 'evaluation.json') or \
            safety_binding.get('source_config_sha256') != sha(run / 'candidate/config.json') or \
            safety_binding.get('returncode') != 0 or not safety.get('cancelled_safely') or \
            not safety.get('after_timeout_reference_matched') or not safety.get('reexport_identical') or \
            safety_binding.get('summary_sha256') != sha(run / 'safety/summary.json'):
        blockers.append('real_cancel_timeout_retry_isolation_not_verified')
    if old is None or old['blockers']:
        blockers.append('old_seven_regression_not_verified_or_failed')
    differences = {}
    for group, before in (('evaluation', baseline), ('development', development_before)):
        if group not in groups:
            continue
        differences[group] = {}
        for key, page in groups[group]['pages'].items():
            differences[group][key] = {v: compare(before[key][v], page[v]) for v in ('original', 'supplementary')}
            for variant, delta in differences[group][key].items():
                if delta['common_order']['counts']['newly_wrong'] or any(
                        delta[k]['newly_missing'] or delta[k]['newly_inexact'] or delta[k]['duplicate_delta'] > 0
                        for k in ('text', 'inline_formula', 'independent_formula', 'table', 'figure')):
                    blockers.append(f'{group}:{key}:{variant}:possible_content_or_order_regression')
    # 不用历史豁免或补充口径覆盖原质量关。
    report = dict(version='issue27-report-1', alignment_version=VERSION,
        role='exposed_development_and_annotated_evaluation_not_independent_holdout',
        execution='historical_diagnostic_only' if args.baseline_only else 'current_candidate_evaluation',
        thresholds_sha256=sha(ROOT / THRESHOLDS_PATH),
        scorer_sha256=sha(Path(__file__)), evaluation=record,
        baseline=dict(pages=baseline, aggregate={v: totals(baseline, v) for v in ('original', 'supplementary')},
                      original_report_sha256=sha(ROOT / 'docs/issue-25/report.json')),
        development_baseline=dict(pages=development_before,
            aggregate={v: totals(development_before, v) for v in ('original', 'supplementary')}),
        groups=groups, changes=differences, old_seven=old,
        engineering_evidence=dict(run=summary, export=export, automated=automated, safety=safety),
        quality_gate=dict(passed=not blockers, blockers=blockers,
                          conclusion='产品质量未达标' if blockers else '本规格范围内产品质量达标'))
    save(args.out, report)
    print(json.dumps(dict(quality_passed=not blockers, blockers=len(blockers)), ensure_ascii=False))
    return 2 if blockers else 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError) as error:
        print(f'输入或冻结证据非法：{error}', file=sys.stderr)
        raise SystemExit(1)
