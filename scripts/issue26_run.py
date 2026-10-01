"""冻结结构候选，通过生产 CLI 实测指定页或完整20页；不复用历史转写。"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import time

from issue20_runtime import freeze_runtime

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return json.loads(path.read_text())


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')


def sha(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli', type=Path, required=True)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--only', nargs='+', help='仅实测指定页；报告明确标为重点页实测')
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    manifest_path = ROOT / 'docs/omnidocbench-20/manifest.json'
    manifest = read(manifest_path)
    data = args.data.resolve()
    if sha(data/'OmniDocBench.json') != manifest['subset_annotation_sha256']:
        raise ValueError('冻结标注哈希不符')
    if len(manifest['pages']) != 20:
        raise ValueError('必须完整运行20页')
    requested = set(args.only or (spec['id'] for spec in manifest['pages']))
    pages = [spec for spec in manifest['pages'] if spec['id'] in requested]
    if {spec['id'] for spec in pages} != requested:
        raise ValueError('指定页不在冻结20页清单中')
    for spec in manifest['pages']:
        if sha(data/spec['image_path']) != spec['image_sha256']:
            raise ValueError(f'{spec["id"]}: 原图哈希不符')
    binary, environment, libraries = freeze_runtime(args.cli.resolve(), out/'.runtime')
    config = read(args.config)
    if config['mode'] != 'production' or config['backend'] != 'mnn:pp-doclayout-v3+ovisocr2':
        raise ValueError('最终验收必须使用真实生产 MNN/Ovis 后端')
    if config['execution']['layout_preprocess'] != 'smartresize_lanczos' or \
       config['execution']['layout_score_threshold'] != .3:
        raise ValueError('本轮冻结 SmartResize Lanczos/0.3')
    models = {}
    for name, model in config['models'].items():
        source = (ROOT/model['root']).resolve()
        target_root = out/'.models'/name
        target_root.mkdir(parents=True)
        for artifact in model['artifacts']:
            original = source/artifact['path']
            if sha(original) != artifact['sha256']:
                raise ValueError(f'模型工件哈希不符：{original}')
            target = target_root/artifact['path']
            target.parent.mkdir(parents=True, exist_ok=True)
            os.link(original, target)
            models[str(target.relative_to(out))] = artifact['sha256']
        model['root'] = str(target_root)
    config_path = out/'.runtime/config.json'
    save(config_path, config)
    sources = {str(p.relative_to(ROOT)): sha(p) for directory in ('src', 'include')
               for p in sorted((ROOT/directory).rglob('*')) if p.is_file()}
    sources['CMakeLists.txt'] = sha(ROOT/'CMakeLists.txt')
    for version in ('1.8', '1.9'):
        for kind in ('image', 'pdf'):
            path = ROOT/f'docs/issue-26/document-ir-{version}-{kind}.schema.json'
            sources[str(path.relative_to(ROOT))] = sha(path)
    freeze = dict(frozen_at_utc=datetime.now(timezone.utc).isoformat(),
                  code_commit=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                  uncommitted_sources=True, source_sha256=sources, runtime=libraries,
                  config_sha256=sha(config_path), source_config_sha256=sha(args.config),
                  model_files=models, model_storage='hash_checked_hardlinks_not_immutable_copies',
                  manifest_sha256=sha(manifest_path), annotation_sha256=manifest['subset_annotation_sha256'],
                  runner_sha256=sha(Path(__file__)), execution='fresh_public_MNN_OvisOCR_inference',
                  scope='focused_pages' if args.only else 'full_20_pages', requested_pages=[p['id'] for p in pages])
    save(out/'freeze.json', freeze)

    def intact():
        return all(sha(ROOT/p) == h for p, h in sources.items()) and \
            sha(config_path) == freeze['config_sha256'] and \
            all(sha(out/'.runtime'/name) == item['sha256'] for name, item in libraries.items()) and \
            all(sha(out/p) == h for p, h in models.items())

    records = []
    for spec in pages:
        if not intact():
            raise ValueError('页执行前候选发生变化，停止验收')
        folder = out/spec['id']
        folder.mkdir()
        image = data/spec['image_path']
        if sha(image) != spec['image_sha256']:
            raise ValueError('页执行前原图发生变化')
        command = [str(binary), '--config', str(config_path), '--input', str(image), '--out', str(folder/'job')]
        record = dict(command=command, input_sha256=spec['image_sha256'], execution=freeze['execution'],
                      provenance=dict(runtime=libraries, config_sha256=freeze['config_sha256'],
                                      source_sha256=sources, freeze_sha256=sha(out/'freeze.json')))
        save(folder/'command.json', record)
        started = time.monotonic()
        with (folder/'stdout.log').open('w') as stdout, (folder/'stderr.log').open('w') as stderr:
            process = subprocess.Popen(command, cwd=ROOT, env=environment, stdout=stdout, stderr=stderr,
                                       start_new_session=True)
            try:
                code = process.wait(timeout=3600)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
                code = 'timeout_3600s'
        record.update(returncode=code, elapsed_seconds=time.monotonic()-started, candidate_intact=intact())
        save(folder/'command.json', record)
        records.append(dict(page_id=spec['id'], returncode=code, candidate_intact=record['candidate_intact'],
                            elapsed_seconds=record['elapsed_seconds']))
        save(out/'progress.json', records)
        print(f'{spec["id"]}: exit={code}, {record["elapsed_seconds"]:.1f}s, '
              f'candidate_intact={record["candidate_intact"]}', flush=True)
        if not record['candidate_intact']:
            raise ValueError('页执行后候选发生变化，停止验收')
    passed = len(records) == len(pages) and all(r['returncode'] == 0 and r['candidate_intact'] for r in records)
    save(out/'run-summary.json', dict(execution=freeze['execution'], completed=len(records),
                                    scope=freeze['scope'], requested_pages=freeze['requested_pages'],
                                    all_jobs_completed=passed, pages=records))
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
