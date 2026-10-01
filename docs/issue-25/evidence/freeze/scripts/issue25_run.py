"""用 #24 的冻结候选新跑 #23 全集，并无损封存每页公共 CLI 原始产物。"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import signal
import shutil
import subprocess
import time
import zipfile

from issue17_baseline import ROOT
from issue25_materials import freeze, frozen_runtime, verify_sources
from issue24_run import save, sha


def run_page(output, jobs, record, candidate, spec):
    candidate_run = Path(record['candidate_run'])
    binary, config, environment = frozen_runtime(candidate_run, candidate)
    folder = jobs / spec['id']
    folder.mkdir()
    if not verify_sources(record, candidate):
        raise ValueError('运行前材料、评分或候选发生变化')
    command = [str(binary), '--config', str(config), '--input', str(ROOT / spec['local_path']),
               '--out', str(folder / 'job')]
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
    receipt = dict(command=command, returncode=code, input_sha256=spec['image_sha256'],
        annotation_sha256=spec['annotation_sha256'], candidate_sha256=record['candidate_sha256'],
        evaluation_sha256=sha(output / 'evaluation.json'), models_intact=verify_sources(record, candidate),
        execution='fresh_public_cli_job', elapsed_seconds=time.monotonic() - started)
    save(folder / 'command.json', receipt)
    destination = output / 'pages' / spec['id']
    shutil.copytree(folder, destination, ignore=shutil.ignore_patterns('assets'))
    files = {str(p.relative_to(folder)): sha(p) for p in sorted(folder.rglob('*')) if p.is_file()}
    archive = output / 'archives' / f"{spec['id']}.zip"
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as bundle:
        for name in files:
            bundle.write(folder / name, name)
    # 校验压缩后每个文件的原字节；没有删改原始输出或丢弃失败页。
    import hashlib
    with zipfile.ZipFile(archive) as bundle:
        if any(hashlib.sha256(bundle.read(name)).hexdigest() != digest for name, digest in files.items()):
            raise ValueError(f"原始产物压缩校验失败：{spec['id']}")
    index = dict(path=str(archive.relative_to(output)), sha256=sha(archive), files_sha256=files)
    save(destination / 'archive.json', index)
    print(spec['id'], code, round(receipt['elapsed_seconds']), flush=True)
    return spec['id'], receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate-run', type=Path, default=ROOT / 'output/issue-24')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--jobs', type=Path, required=True, help='新作业暂存目录，可位于内存文件系统')
    parser.add_argument('--workers', type=int, choices=(1, 2), default=1)
    args = parser.parse_args()
    output, jobs = args.out.resolve(), args.jobs.resolve()
    output.mkdir(parents=True, exist_ok=False)
    jobs.mkdir(parents=True, exist_ok=False)
    (output / 'archives').mkdir()
    record, candidate, manifest = freeze(output, args.candidate_run.resolve())
    save(output / 'integrity-before.json', dict(checked_at_utc=datetime.now(timezone.utc).isoformat(),
        passed=verify_sources(record, candidate), evaluation_sha256=sha(output / 'evaluation.json')))
    receipts = {}
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = [pool.submit(run_page, output, jobs, record, candidate, spec) for spec in manifest['pages']]
        for future in as_completed(futures):
            key, receipt = future.result()
            receipts[key] = receipt
            save(output / 'progress.json', receipts)
    intact = verify_sources(record, candidate)
    save(output / 'run-summary.json', dict(workers=args.workers, expected=len(manifest['pages']),
        completed=len(receipts), failed=[k for k, r in receipts.items() if r['returncode'] != 0],
        candidate_and_materials_intact=intact, evaluation_sha256=sha(output / 'evaluation.json'),
        completed_at_utc=datetime.now(timezone.utc).isoformat()))
    return int(not intact or any(r['returncode'] != 0 for r in receipts.values()))


if __name__ == '__main__':
    raise SystemExit(main())
