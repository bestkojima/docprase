"""冻结候选并重跑20页开发集及旧7页；失败页继续留档，不复用历史结果。"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import signal
import shutil
import subprocess
import time

from issue17_baseline import DATA, MANIFEST, ROOT, verify_data
from issue20_runtime import freeze_runtime


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def candidate_intact(output, record):
    files = dict(record['model_files'])
    files['candidate/config.json'] = record['effective_config_sha256']
    files['candidate/thresholds.json'] = record['thresholds_sha256']
    files.update({f'candidate/runtime/{name}': item['sha256'] for name, item in record['runtime'].items()})
    return all(sha(output / name) == digest for name, digest in files.items())


def freeze(cli, config_path, output, thresholds):
    runtime = output / 'candidate'
    binary, environment, libraries = freeze_runtime(cli.resolve(), runtime / 'runtime')
    config = json.loads(config_path.read_text())
    model_files = {}
    for name, model in config['models'].items():
        source = (ROOT / model['root']).resolve()
        destination = runtime / 'models' / name
        destination.mkdir(parents=True)
        for artifact in model['artifacts']:
            original = source / artifact['path']
            if sha(original) != artifact['sha256']:
                raise ValueError(f'模型工件哈希不符：{original}')
            target = destination / artifact['path']
            target.parent.mkdir(parents=True, exist_ok=True)
            # 同文件系统硬链接不增加工件存储；每页前后仍核对实际字节。
            os.link(original, target)
            model_files[str(target.relative_to(output))] = artifact['sha256']
        model['root'] = str(destination)
    snapshot_config = runtime / 'config.json'
    save(snapshot_config, config)
    shutil.copy2(thresholds, runtime / 'thresholds.json')
    sources = {str(p.relative_to(ROOT)): sha(p)
               for directory in ('src', 'include') for p in sorted((ROOT / directory).rglob('*'))
               if p.is_file()}
    sources['CMakeLists.txt'] = sha(ROOT / 'CMakeLists.txt')
    record = dict(frozen_at_utc=datetime.now(timezone.utc).isoformat(),
        code_commit=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
        source_sha256=sources, source_config_sha256=sha(config_path),
        effective_config_sha256=sha(snapshot_config), runtime=libraries,
        model_files=model_files, model_storage='sha_locked_hardlink_references',
        thresholds_sha256=sha(thresholds), runner_sha256=sha(Path(__file__)),
        dataset_manifest_sha256=sha(MANIFEST), old_manifest_sha256=sha(ROOT / 'docs/issue-15/samples.json'),
        execution='fresh_public_cli_jobs', system=dict(uname=list(os.uname())))
    save(output / 'candidate.json', record)
    return binary, snapshot_config, environment


def run_sample(binary, config, environment, output, group, spec):
    folder = output / group / spec['id']
    folder.mkdir(parents=True)
    source = DATA / spec['image_path'] if group == 'development' else ROOT / spec['path']
    expected = spec.get('image_sha256', spec.get('sha256'))
    if sha(source) != expected:
        raise ValueError(f'输入哈希不符：{source}')
    frozen = json.loads((output / 'candidate.json').read_text())
    if not candidate_intact(output, frozen):
        raise ValueError('运行前候选哈希变化，拒绝执行')
    command = [str(binary), '--config', str(config), '--input', str(source), '--out', str(folder / 'job')]
    if source.suffix == '.pdf':
        command += ['--pages', '1-2', '--dpi', '200']
    started = time.monotonic()
    with (folder / 'stdout.log').open('w') as stdout, (folder / 'stderr.log').open('w') as stderr:
        process = subprocess.Popen(['/usr/bin/time', '-v', '-o', str(folder / 'time.txt'), *command],
            cwd=ROOT, env=environment, stdout=stdout, stderr=stderr, start_new_session=True)
        try:
            code = process.wait(timeout=3600)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            code = 'timeout_3600s'
    record = dict(command=command, input_sha256=expected, returncode=code,
        models_intact=candidate_intact(output, frozen), elapsed_seconds=time.monotonic() - started,
        execution='fresh_public_cli_job', candidate_sha256=sha(output / 'candidate.json'))
    save(folder / 'command.json', record)
    print(group, spec['id'], code, round(record['elapsed_seconds']), flush=True)
    return f"{group}/{spec['id']}", record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli', required=True, type=Path)
    parser.add_argument('--config', type=Path, default=ROOT / 'configs/printed-page.example.json')
    parser.add_argument('--thresholds', type=Path, default=ROOT / 'docs/issue-24/thresholds.json')
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--workers', type=int, choices=(1, 2), default=1,
        help='独立进程并行；每个模型仍使用冻结的单线程配置，不用于性能验收')
    args = parser.parse_args()
    manifest = json.loads(MANIFEST.read_text())
    verify_data(manifest, json.loads((DATA / 'OmniDocBench.json').read_text()))
    old = json.loads((ROOT / 'docs/issue-15/samples.json').read_text())
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=False)
    binary, config, environment = freeze(args.cli, args.config, output, args.thresholds)
    records = {}
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = [pool.submit(run_sample, binary, config, environment, output, group, spec)
            for group, specs in (('development', manifest['pages']), ('old-seven', old['samples']))
            for spec in specs]
        for future in as_completed(futures):
            key, record = future.result()
            records[key] = record
            save(output / 'progress.json', records)
    frozen = json.loads((output / 'candidate.json').read_text())
    intact = candidate_intact(output, frozen)
    save(output / 'run-summary.json', dict(candidate_intact=intact, workers=args.workers,
        completed=len(records), failed=[k for k, r in records.items() if r['returncode'] != 0]))
    return int(not intact or any(r['returncode'] != 0 for r in records.values()))


if __name__ == '__main__':
    raise SystemExit(main())
