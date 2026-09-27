"""CLI 通过公共作业接口实时记录阶段、信号取消和协作超时。"""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

from cli_integration import png_2x2
from printed_page_integration import ROOT, config


def main():
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        source = root / 'source.png'
        source.write_bytes(png_2x2())
        setting = root / 'config.json'
        setting.write_text(json.dumps(config('printed_page_gate')))
        for mode, expected, state in [('signal', 130, 'cancelled'),
                                      ('timeout', 6, 'timed_out')]:
            gate = root / (mode + '.gate')
            entered = root / (mode + '.entered')
            gate.touch()
            env = dict(os.environ, DOCOCR_TEST_GATE_PATH=str(gate),
                       DOCOCR_TEST_ENTERED_PATH=str(entered))
            output = root / mode
            cmd = [binary, '--config', str(setting), '--input', str(source),
                   '--out', str(output)]
            if mode == 'timeout':
                cmd += ['--timeout-ms', '500']
            process = subprocess.Popen(cmd, cwd=ROOT, env=env,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline and not entered.exists():
                time.sleep(0.01)
            assert entered.exists(), (mode, process.poll())
            if mode == 'signal':
                process.send_signal(signal.SIGINT)
                expected_event = 'cancel_requested'
            else:
                expected_event = 'timeout_requested'
            while time.monotonic() < deadline:
                events_file = output / 'job-events.jsonl'
                if events_file.exists() and expected_event in events_file.read_text():
                    break
                time.sleep(0.01)
            assert expected_event in events_file.read_text()
            gate.unlink()
            stdout, stderr = process.communicate(timeout=8)
            assert process.returncode == expected, (mode, process.returncode, stdout, stderr)
            status = json.loads((output / 'job-status.json').read_text())
            assert status['state'] == state and status['terminal']
            assert status['page_completed'] == 0
            events = [json.loads(line) for line in (output / 'job-events.jsonl').read_text().splitlines()]
            assert any(item['kind'] == 'region_started' for item in events)
            assert events[-1]['kind'] == 'terminal'
            assert state in stderr


if __name__ == '__main__':
    main()
