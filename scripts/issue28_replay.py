"""校验已有公共作业的来源，再重跑参照追踪；不重新调用模型。"""
import argparse
import json
from pathlib import Path

from issue28_audit import compare_job, validate_controlled_job
from issue28_reference import ROOT, load_reference, save, sha


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--from-report', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--reference', type=Path, default=ROOT / '.scratch/issue28-reference')
    args = parser.parse_args()
    source = json.loads(args.from_report.read_text())
    env, labels = load_reference(args.reference)
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    result = dict(execution='same_captured_public_jobs_reference_replay_no_new_model_inference',
                  source_report=str(args.from_report.resolve()), source_report_sha256=sha(args.from_report),
                  scripts={name: sha(ROOT/'scripts'/name) for name in
                           ('issue28_reference.py', 'issue28_audit.py', 'issue28_replay.py')}, entries={})
    for name, entry in {**source['cases'], **source['real']}.items():
        previous_path = Path(entry['path'])
        if sha(previous_path) != entry['sha256']:
            raise ValueError(f'{name}: 原比较记录变化')
        previous = json.loads(previous_path.read_text())
        job = previous_path.parent/'job'
        if sha(job/'document.json') != previous['document_sha256'] or \
                sha(job/'run-manifest.json') != previous['run_manifest_sha256']:
            raise ValueError(f'{name}: 原公共作业产物变化')
        for asset in previous['raw_assets'].values():
            if sha(job/asset['path']) != asset['sha256']:
                raise ValueError(f'{name}: 原输入/输出张量变化')
        current = compare_job(job, env, labels)
        for mode in ('rect', 'auto'):
            current_boxes = json.loads(json.dumps(current[mode]['pipeline_output'], default=lambda x: x.tolist())) \
                if mode in current else None
            if mode in previous and current_boxes != previous[mode]['pipeline_output']:
                raise ValueError(f'{name}: 原官方输出变化；不能视为单纯追踪修复')
        checks = validate_controlled_job(job, name) if name in source['cases'] else []
        if name in ('short_title', 'reference_skip'):
            expected = 'short_box' if name == 'short_title' else 'reference_label'
            for mode in ('rect', 'auto'):
                if current[mode]['candidates'][0]['removal_reason'] != expected:
                    raise ValueError(f'{name}: 外层删除原因错误')
        target = out/(name+'.json')
        save(target, current)
        result['entries'][name] = dict(path=str(target), sha256=sha(target), source_job=str(job),
                                      public_behavior_checks=checks, official_outputs_unchanged=True)
        print(name, 'passed', flush=True)
    save(out/'report.json', result)


if __name__ == '__main__':
    main()
