"""从 model_probe 入口验证可见停止状态和超时清理。"""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "scripts/model_probe_ovis.py"
SAMPLE = ROOT / "tests/fixtures/ovis/chinese_text.png"


class OvisProbeTests(unittest.TestCase):
    def invoke(self, folder, runner_source, max_tokens=32, timeout=3):
        folder = Path(folder)
        runner = folder / "runner.py"
        runner.write_text("#!/usr/bin/env python3\n" + runner_source)
        runner.chmod(0o755)
        out = folder / "result"
        process = subprocess.run(
            [sys.executable, str(ENTRY), "--out", str(out), "--image", str(SAMPLE),
             "--runner", str(runner), "--max-tokens", str(max_tokens),
             "--timeout", str(timeout)], cwd=ROOT, capture_output=True, text=True,
        )
        return process, json.loads((out / "report.json").read_text()), out

    def test_zero_exit_at_token_cap_is_incomplete(self):
        with tempfile.TemporaryDirectory() as folder:
            process, report, out = self.invoke(folder, """
import json,sys
from pathlib import Path
Path(sys.argv[5]).write_text('未完的表格 <table><tr>')
print('OVIS_PROBE '+json.dumps({'status':'MAX_TOKENS_FINISHED','tokens':32,'vision_us':50000,'pixels_mp':0.1}))
""")
            self.assertEqual(process.returncode, 0)
            self.assertEqual(report["stop_reason"], "token_limit")
            self.assertEqual(report["completeness"], "incomplete")
            self.assertEqual((out / "raw.txt").read_text(), "未完的表格 <table><tr>")

    def test_normal_stop_requires_actual_vision_work(self):
        with tempfile.TemporaryDirectory() as folder:
            _, report, _ = self.invoke(folder, """
import json,sys
from pathlib import Path
Path(sys.argv[5]).write_text('看似完整')
print('OVIS_PROBE '+json.dumps({'status':'NORMAL_FINISHED','tokens':8,'vision_us':0,'pixels_mp':0}))
""")
            self.assertEqual(report["stop_reason"], "error")
            self.assertEqual(report["completeness"], "incomplete")

    def test_timeout_kills_child_process_group(self):
        with tempfile.TemporaryDirectory() as folder:
            process, report, _ = self.invoke(folder, """
import subprocess,sys,time
from pathlib import Path
p=subprocess.Popen([sys.executable,'-c','import time;time.sleep(60)'])
Path(sys.argv[5]+'.childpid').write_text(str(p.pid))
time.sleep(60)
""", timeout=1)
            self.assertNotEqual(process.returncode, 0)
            self.assertEqual(report["stop_reason"], "timeout")
            self.assertEqual(report["completeness"], "incomplete")
            self.assertTrue(report["process_group_terminated"])

    def test_timeout_kills_term_ignoring_child_after_leader_exits(self):
        with tempfile.TemporaryDirectory() as folder:
            process = None
            child_pid = None
            try:
                process, report, out = self.invoke(folder, """
import subprocess,sys,time
from pathlib import Path
child = subprocess.Popen([sys.executable, '-c',
    'import os,signal,time;signal.signal(signal.SIGTERM,signal.SIG_IGN);time.sleep(60)'],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
Path(sys.argv[5]+'.childpid').write_text(str(child.pid))
time.sleep(60)
""", timeout=1)
                child_pid = int((out / "raw.txt.childpid").read_text())
                self.assertNotEqual(process.returncode, 0)
                self.assertEqual(report["stop_reason"], "timeout")
                self.assertTrue(report["process_group_terminated"])
                stat = Path("/proc") / str(child_pid) / "stat"
                self.assertTrue(not stat.exists() or stat.read_text().rsplit(") ", 1)[1][0] == "Z")
            finally:
                if child_pid is not None:
                    try:
                        os.kill(child_pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass

    def test_suite_rejects_normal_eos_when_formula_or_table_format_is_missing(self):
        with tempfile.TemporaryDirectory() as folder:
            runner = Path(folder) / "runner.py"
            runner.write_text("""#!/usr/bin/env python3
import json,sys
from pathlib import Path
Path(sys.argv[5]).write_text('看似完成')
vision = 0 if sys.argv[2].endswith('invalid.png') else 10000
print('OVIS_PROBE '+json.dumps({'status':'NORMAL_FINISHED','tokens':4,'vision_us':vision,'pixels_mp':0.1 if vision else 0}))
""")
            runner.chmod(0o755)
            out = Path(folder) / "suite"
            process = subprocess.run(
                [sys.executable, str(ENTRY), "--suite", "--out", str(out),
                 "--runner", str(runner), "--max-tokens", "32", "--timeout", "3"],
                cwd=ROOT, capture_output=True, text=True,
            )
            self.assertNotEqual(process.returncode, 0)
            summary = json.loads((out / "suite-report.json").read_text())
            self.assertEqual(summary["overall"], "diagnostic_failed")
            self.assertFalse(summary["checks"]["sample_generation_and_table_structure"])


if __name__ == "__main__":
    unittest.main()
