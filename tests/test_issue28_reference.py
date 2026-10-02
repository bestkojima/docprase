"""冻结官方完整入口的行为回归；通过核验 CLI 观察来源和顺序。"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ReferenceCLI(unittest.TestCase):
    def run_reference_cli(self, value):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            source, out = folder / 'input.json', folder / 'report.json'
            source.write_text(json.dumps(value))
            result = subprocess.run([sys.executable, str(ROOT / 'scripts/issue28_reference.py'),
                                     '--input', str(source), '--out', str(out),
                                     '--reference', str(ROOT / '.scratch/issue28-reference')],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(out.read_text())

    def test_equal_scores_do_not_erase_source_identity(self):
        report = self.run_reference_cli(dict(size=[200, 200], unclip=[1.5, 2.0], rows=[
            [22, .9, 10, 10, 30, 30, 2], [22, .9, 100, 100, 120, 120, 0]]))
        for mode in ('rect', 'auto'):
            self.assertEqual([b['candidate_id'] for b in report[mode]['pipeline_output']], [1, 0])
            self.assertEqual(report[mode]['pipeline_output'][1]['coordinate'], [5, 0, 35, 40])

    def test_rank_mask_and_pipeline_order_survive_filtering(self):
        report = self.run_reference_cli(dict(size=[200, 200], threshold=.3, rows=[
            [22, .9, 10, 10, 90, 50, 2],
            [15, .8, 20, 20, 80, 40, 0],
            [17, .7, 100, 80, 180, 84, 1]],
            mask_rectangles=[[10, 10, 90, 50], [20, 20, 80, 40], [100, 80, 180, 84]]))
        for mode in ('rect', 'auto'):
            trace = report[mode]
            self.assertEqual(trace['stages']['rank']['candidate_ids'], [1, 2, 0])
            self.assertEqual([b['candidate_id'] for b in trace['pipeline_output']], [0])
            # 管线先编号再过滤，保留间隙；独立模型入口在过滤后重新编号。
            self.assertEqual(trace['pipeline_output'][0]['order'], 3)
            self.assertEqual(trace['standalone_output'][0]['order'], 1)
            self.assertEqual(trace['candidates'][1]['removed_at'], 'outer_overlap')
            self.assertEqual(trace['candidates'][2]['removed_at'], 'outer_overlap')
            self.assertEqual(trace['candidates'][1]['removal_reason'], 'inline_formula_overlap')
            self.assertEqual(trace['candidates'][1]['related_candidate_id'], 0)
            self.assertEqual(trace['candidates'][2]['removal_reason'], 'short_box')
        self.assertEqual(report['auto']['stages']['rank']['mask_rows'], [1, 2, 0])
        self.assertEqual(report['labels'][15], 'inline_formula')


if __name__ == '__main__':
    unittest.main()
