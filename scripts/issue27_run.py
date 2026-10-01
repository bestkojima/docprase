"""同一冻结候选串行新跑20页、旧7页及12页带标注全集。"""
import argparse
from datetime import datetime, timezone
import signal
import subprocess
import time
from pathlib import Path
import os

from issue17_baseline import ROOT
from issue24_run import freeze, save, sha
from issue27_evidence import check_frozen, freeze_evaluation, suite_specs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli', type=Path, required=True)
    parser.add_argument('--config', type=Path, default=ROOT / 'configs/printed-page.example.json')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    jobs, manifest = suite_specs()
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=False)
    binary, config, environment = freeze(args.cli, args.config, output, ROOT / 'docs/issue-24/thresholds.json')
    freeze_evaluation(output, jobs, manifest, binary, environment)
    record, *_ = check_frozen(output, Path(__file__))
    save(output / 'integrity-before.json', dict(passed=True, evaluation_sha256=sha(output / 'evaluation.json')))
    receipts = {}
    for job in jobs:
        check_frozen(output, Path(__file__))
        folder = output / job['key']
        folder.mkdir(parents=True)
        command = [str(binary), '--config', str(config), '--input', job['source'], '--out', str(folder / 'job')]
        if Path(job['source']).suffix.lower() == '.pdf':
            command += ['--pages', '1-2', '--dpi', '200']
        receipt = dict(command=command, input_sha256=job['input_sha256'], returncode='not_run',
            candidate_sha256=record['candidate_sha256'], evaluation_sha256=sha(output / 'evaluation.json'),
            execution='fresh_public_cli_job')
        save(folder / 'command.json', receipt)
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
        check_frozen(output, Path(__file__))
        files = {str(p.relative_to(folder)): sha(p) for p in sorted(folder.rglob('*'))
                 if p.is_file() and p.name != 'command.json'}
        receipt.update(returncode=code, elapsed_seconds=time.monotonic() - started,
                       models_intact=True, files_sha256=files)
        save(folder / 'command.json', receipt)
        receipts[job['key']] = receipt
        save(output / 'progress.json', receipts)
        print(job['key'], code, round(receipt['elapsed_seconds']), flush=True)
    check_frozen(output, Path(__file__))
    save(output / 'run-summary.json', dict(completed=len(receipts), expected=record['expected_jobs'],
        expected_pages=record['expected_pages'], candidate_and_materials_intact=True,
        evaluation_sha256=sha(output / 'evaluation.json'),
        failed=[k for k, r in receipts.items() if r['returncode'] != 0],
        completed_at_utc=datetime.now(timezone.utc).isoformat()))
    return int(any(r['returncode'] != 0 for r in receipts.values()))


if __name__ == '__main__':
    raise SystemExit(main())
