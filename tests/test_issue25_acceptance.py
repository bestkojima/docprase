"""通过公开评分命令验证缺失或失败结果不能获得质量通过。"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AcceptanceCommandTests(unittest.TestCase):
    def test_changed_frozen_rules_are_rejected_before_scoring(self):
        with tempfile.TemporaryDirectory(prefix='issue25-test-') as directory:
            run = Path(directory)
            frozen = run / 'freeze' / 'docs/issue-24/thresholds.json'
            frozen.parent.mkdir(parents=True)
            frozen.write_text('{}\n')
            (run / 'evaluation.json').write_text(json.dumps({
                'snapshot_files': {'docs/issue-24/thresholds.json': '0' * 64}}))
            report = run / 'report.json'
            result = subprocess.run([sys.executable, str(ROOT / 'scripts/issue25_report.py'),
                '--run', str(run), '--out', str(report)], cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn('SHA', result.stderr)
            self.assertFalse(report.exists())

    def test_unexecuted_pages_keep_all_reference_denominators(self):
        with tempfile.TemporaryDirectory(prefix='issue25-test-') as directory:
            run = Path(directory)
            report = run / 'report.json'
            result = subprocess.run([sys.executable, str(ROOT / 'scripts/issue25_report.py'),
                '--run', str(run), '--out', str(report)], cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 2, result.stderr)
            value = json.loads(report.read_text())
            self.assertEqual(len(value['pages']), 12)
            self.assertEqual(value['aggregate']['text']['reference_chars'], 11685)
            self.assertEqual(value['aggregate']['text']['edit_distance'], 11685)
            self.assertEqual(value['aggregate']['pages_failed'], 12)
            self.assertEqual(value['aggregate']['text']['denominator'], 182)
            self.assertEqual(value['aggregate']['inline_formula']['denominator'], 83)
            self.assertEqual(value['aggregate']['independent_formula']['denominator'], 8)
            self.assertEqual(value['aggregate']['table']['denominator'], 4)
            self.assertEqual(value['aggregate']['table']['cells'], 36)
            self.assertEqual(value['aggregate']['figure']['denominator'], 28)
            self.assertEqual(value['aggregate']['reading_order']['denominator'], 1675)
            self.assertFalse(value['quality_gate']['passed'])
            self.assertEqual(value['quality_gate']['metrics']['text_cer']['value'], 1.0)
            self.assertEqual(value['quality_gate']['metrics']['missing_rate']['value'], 1.0)


if __name__ == '__main__':
    unittest.main()
