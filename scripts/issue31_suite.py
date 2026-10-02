"""#31 冻结全集、生产导出与八项质量验收。"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from issue17_baseline import ROOT
from issue24_run import save, sha
from issue24_report import audit, old_seven, quality_metrics, compare
from issue25_materials import restore_page
from issue27_evidence import check_frozen as check_legacy, read, freeze_evaluation as freeze_legacy, system_runtime
from issue31_evaluation import evaluate_page, totals, gates, VERSION

TOOLS = ('scripts/issue31_run.py', 'scripts/issue31_evaluation.py', 'scripts/issue31_suite.py')


def check_frozen(run):
    result = check_legacy(run)
    record = result[0]
    if system_runtime(Path(record['rendering_node']), result[4]) != record['rendering_runtime_sha256']:
        raise ValueError('实际数学渲染运行时变化')
    return result


def load_job(run, spec, record):
    folder = run / spec['key']
    receipt = read(folder / 'command.json')
    expected = dict(candidate_sha256=record['candidate_sha256'], evaluation_sha256=sha(run/'evaluation.json'),
                    input_sha256=spec['input_sha256'], execution='fresh_public_cli_job', returncode=0,
                    models_intact=True)
    if any(receipt.get(key) != value for key, value in expected.items()):
        raise ValueError('缺少成功的新推理或冻结绑定')
    inventory = receipt.get('files_sha256', {})
    if not {'job/document.json', 'job/document.md'} <= inventory.keys():
        raise ValueError('导出产物未绑定原始作业收据')
    for name, digest in inventory.items():
        source = (folder / name).resolve()
        if not source.is_relative_to(folder.resolve()) or sha(source) != digest:
            raise ValueError(f'原始产物 SHA 不符：{name}')
    document = read(folder / 'job/document.json')
    if any('job/' + r['path'] not in inventory for r in document.get('resources', [])):
        raise ValueError('资源未绑定原始作业收据')
    return document


def freeze_evaluation(output, jobs, manifest, binary, environment):
    record = freeze_legacy(output, jobs, manifest, binary, environment)
    paths = [ROOT / name for name in TOOLS]
    paths += list((ROOT / 'schemas/document-ir').glob('*.json'))
    paths += list((ROOT / 'tests/math-rendering').glob('*.mjs'))
    paths += [ROOT / 'tests/math-rendering/package-lock.json']
    paths += [ROOT / 'tests/test_issue31_evaluation.py']
    paths += [p for p in (ROOT / 'tests/math-rendering/node_modules').rglob('*') if p.is_file()]
    for path in paths:
        name = str(path.relative_to(ROOT))
        target = output / 'freeze' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
        record['source_files'][name] = record['snapshot_files'][name] = sha(path)
    node = Path(shutil.which('node')).resolve()
    record.update(version='issue31-evaluation-1', scoring_version=VERSION,
                  rendering_node=str(node), rendering_runtime_sha256=system_runtime(node, environment))
    save(output / 'evaluation.json', record)
    return record


def verify_exports(run):
    import jsonschema
    from PIL import Image
    record, candidate, binary, _, environment = check_frozen(run)
    records = {}
    for spec in record['jobs']:
        folder = run / spec['key']
        job = folder / 'job'
        errors, resources, pages, renders = [], {}, 0, []
        try:
            document = load_job(run, spec, record)
            schema = run / f"freeze/schemas/document-ir/document-ir-{document['schema_version']}-{document['source']['type']}.schema.json"
            jsonschema.validate(document, read(schema))
            errors.extend(audit(document, job)['errors'])
            for resource in document['resources']:
                source = (job / resource['path']).resolve()
                if not source.is_relative_to(job.resolve()):
                    raise ValueError('资源路径越界')
                with Image.open(source) as image:
                    if image.size != (resource['width'], resource['height']):
                        raise ValueError('资源尺寸不符')
                    image.verify()
                resources[resource['path']] = sha(source)
            with tempfile.TemporaryDirectory(prefix='issue31-export-') as temporary:
                exported = Path(temporary) / 'reexport'
                result = subprocess.run([str(binary), '--reexport', str(job / 'document.json'),
                    '--asset-root', str(job), '--out', str(exported)], env=environment, capture_output=True, text=True)
                if result.returncode:
                    raise ValueError(result.stderr)
                for name in ('document.json', 'document.md', *resources):
                    if sha(job / name) != sha(exported / name):
                        raise ValueError(f'重新导出字节不同：{name}')
                original = Path(temporary) / 'original'
                original.mkdir()
                shutil.copy2(job / 'document.md', original / 'document.md')
                for label, folder in [('first_export', original), ('reexport', exported)]:
                    rendered = subprocess.run([record['rendering_node'], str(ROOT / 'tests/math-rendering/preview.mjs'),
                        str(folder / 'document.md')], env=environment, capture_output=True, text=True)
                    renders.append(dict(kind=label, returncode=rendered.returncode, stderr=rendered.stderr,
                        html_sha256=sha(folder / 'document.html') if rendered.returncode == 0 else None))
                    if rendered.returncode:
                        errors.append(f'{label}:math_render_failed')
            pages = len(document['pages'])
            if pages != spec['spec'].get('pages', 1):
                raise ValueError('页数不符')
        except (OSError, ValueError, KeyError, jsonschema.ValidationError) as error:
            errors.append(str(error))
        records[spec['key']] = dict(errors=errors, pages_checked=pages,
                                   resources_sha256=resources, rendering=renders)
        print(spec['key'], 'PASS' if not errors else errors, flush=True)
    check_frozen(run)
    result = dict(passed=len(records)==38 and sum(r['pages_checked'] for r in records.values())==39
                  and not any(r['errors'] for r in records.values()), records=records,
                  pages_checked=sum(r['pages_checked'] for r in records.values()),
                  candidate_sha256=record['candidate_sha256'], evaluation_sha256=sha(run/'evaluation.json'),
                  cli_sha256=candidate['runtime']['dococr_cli']['sha256'])
    save(run/'issue31-export-verification.json',result)
    return 0 if result['passed'] else 2


def report(run, output):
    record, candidate, _, _, _ = check_frozen(run)
    groups = {}
    for group in ('development', 'evaluation'):
        pages, baseline = {}, {}
        for spec in record['jobs']:
            if not spec['key'].startswith(group+'/'):
                continue
            folder=run/spec['key']; errors=[]
            try:
                document=load_job(run, spec, record)
                errors.extend(audit(document,folder/'job')['errors'])
            except (OSError, ValueError, KeyError) as error:
                errors.append(str(error))
            if errors:
                document=dict(status='failed',pages=[])
            value=evaluate_page(spec['annotation'],document)
            value.update(returncode=0 if not errors else 'invalid_or_missing_execution',errors=errors,
                evidence=str(folder), input_sha256=spec['input_sha256'])
            pages[spec['spec']['id']]=value
            if group == 'development':
                previous = ROOT / 'docs/issue-24/evidence' / spec['key'] / 'job/document.json'
                before_document = read(previous)
                before_sha = sha(previous)
            else:
                with restore_page(ROOT / 'output/issue-25', spec['spec']['id']) as previous:
                    before_document = read(previous / 'job/document.json')
                    before_sha = sha(previous / 'job/document.json')
            before = evaluate_page(spec['annotation'], before_document)
            before.update(document_sha256=before_sha, execution='historical_output_rescore')
            baseline[spec['spec']['id']] = before
        current=totals(pages); historical=totals(pages,'original')
        groups[group]=dict(pages=pages,aggregate=current,metrics=gates(current),
                           baseline=dict(pages=baseline, aggregate=totals(baseline), metrics=gates(totals(baseline))),
                           changes={key:compare(baseline[key]['parent'], pages[key]['parent']) for key in pages},
                           historical_nine_metrics=quality_metrics(historical),historical_aggregate=historical)
    blockers=[f'{group}:{key}:threshold_not_met' for group,g in groups.items()
              for key,m in g['metrics'].items() if not m['passed']]
    blockers += [f'{group}:{key}:content_unverified' for group,g in groups.items()
                 for key,p in g['pages'].items() if not p['content_audit']['verified']]
    summary=read(run/'run-summary.json') if (run/'run-summary.json').exists() else {}
    if summary.get('completed')!=38 or summary.get('expected_pages')!=39 or summary.get('failed') or \
        summary.get('evaluation_sha256')!=sha(run/'evaluation.json'):
        blockers.append('full_new_inference_not_verified')
    engineering={}
    for name in ('issue31-export-verification.json','automated-verification.json','safety-verification.json'):
        value=read(run/name) if (run/name).exists() else {}
        engineering[name]=value
        if value.get('candidate_sha256')!=record['candidate_sha256'] or \
            value.get('evaluation_sha256')!=sha(run/'evaluation.json') or not value.get('passed'):
            blockers.append(name+':not_verified')
    automated=engineering['automated-verification.json']
    if automated.get('cli_sha256')!=candidate['runtime']['dococr_cli']['sha256']:
        blockers.append('automated_binary_not_bound')
    safety=engineering['safety-verification.json']
    safety_path=run/'safety/summary.json'
    evidence=read(safety_path) if safety_path.exists() else {}
    if not safety_path.exists() or safety.get('summary_sha256')!=sha(safety_path) or \
        safety.get('source_config_sha256')!=sha(run/'candidate/config.json') or \
        safety.get('returncode')!=0 or not all(evidence.get(k) for k in
            ('cancelled_safely','after_timeout_reference_matched','reexport_identical')):
        blockers.append('real_safety_not_verified')
    old=old_seven(run,record['candidate_sha256']) if summary.get('completed')==38 else None
    if not old or old['blockers']:
        blockers.append('old_seven_not_verified_or_failed')
    result=dict(version=VERSION, execution='fresh_frozen_candidate',evaluation_sha256=sha(run/'evaluation.json'),
        candidate_sha256=record['candidate_sha256'],groups=groups,engineering=engineering,old_seven=old,
        quality_gate=dict(passed=not blockers,blockers=blockers),
        strict_independent_holdout_claim=False)
    save(output,result)
    print(json.dumps(dict(passed=not blockers, blockers=len(blockers))))
    return 0 if not blockers else 2


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action',choices=('verify-exports','report'))
    parser.add_argument('--run',type=Path,required=True)
    parser.add_argument('--out',type=Path)
    args=parser.parse_args()
    run=args.run.resolve()
    return verify_exports(run) if args.action=='verify-exports' else report(run,args.out or run/'issue31-report.json')


if __name__=='__main__':
    try:
        raise SystemExit(main())
    except (OSError,ValueError,KeyError) as error:
        print(f'冻结或输入证据非法：{error}',file=sys.stderr)
        raise SystemExit(1)
