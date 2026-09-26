"""model_probe_layout 公开诊断行为的无模型回归。"""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np
from PIL import Image

from scripts.model_probe_layout import evaluate_candidates, map_mask_to_page, reference_page_mask

ROOT = Path(__file__).resolve().parents[1]
REAL_MODEL = ROOT / 'models/doclayout/PP-DocLayoutV3.mnn'


class LayoutProbeTests(unittest.TestCase):
    def test_candidates_keep_row_identity_and_reasons(self):
        boxes = np.array([
            [22, .9, 10, 10, 40, 40, 7],
            [22, .8, -2, 10, 40, 40, 7],
            [22, .1, 10, 10, 40, 40, 90],
            [22, .7, 20, 20, 20, 40, 91],
        ], dtype=np.float32)
        masks = np.zeros((4, 200, 200), dtype=np.int32)
        records = evaluate_candidates(boxes, masks, .5, (100, 100))
        self.assertEqual([r['candidate_id'] for r in records], [0, 1, 2, 3])
        self.assertEqual([r['reasons'] for r in records], [[], ['outside_page'], ['below_score_threshold'], ['degenerate_box']])
        self.assertEqual([r['rank'] for r in records[:2]], [7, 7])
        self.assertEqual([r['mask_row'] for r in records], [0, 1, 2, 3])

    def test_mask_maps_into_original_box(self):
        mask = np.zeros((200, 200), dtype=np.uint8)
        mask[50:70, 70:90] = 1
        page = map_mask_to_page(mask, [120, 80, 200, 160], (400, 400))
        self.assertEqual(page.shape, (400, 400))
        self.assertGreater(int(page.sum()), 0)
        self.assertEqual(int(page[:80].sum() + page[160:].sum()), 0)
        self.assertEqual(int(page[:, :120].sum() + page[:, 200:].sum()), 0)

    def test_mask_matches_reference_on_nonsquare_page_and_clipped_box(self):
        mask = np.zeros((200, 200), dtype=np.uint8)
        mask[0:55, 145:200] = 1
        for box in ([420, -10, 610, 160], [0, 0, 120, 80]):
            actual = map_mask_to_page(mask, box, (360, 600))
            reference = reference_page_mask(mask, box, (360, 600))
            np.testing.assert_array_equal(actual, reference)

    @unittest.skipUnless(REAL_MODEL.is_file(), '需要指定真实 MNN 模型')
    def test_reference_hash_must_match_image(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder) / 'result'
            wrong = Path(folder) / 'wrong.json'
            wrong.write_text(json.dumps({'image_sha256': 'wrong', 'annotations': []}))
            p = subprocess.run([sys.executable, str(ROOT / 'scripts/model_probe_layout.py'),
                '--image', str(ROOT / 'tests/fixtures/layout/exam-jee-346.jpg'),
                '--reference', str(wrong), '--out', str(out)], capture_output=True, text=True)
            self.assertNotEqual(p.returncode, 0)
            report = json.loads((out / 'report.json').read_text())
            self.assertIn('reference_sample_hash_mismatch', report['errors'])

    def test_missing_model_writes_failed_report(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder) / 'result'
            p = subprocess.run([sys.executable, str(ROOT / 'scripts/model_probe_layout.py'),
                '--model', str(Path(folder) / 'missing.mnn'), '--image', str(ROOT / 'tests/fixtures/layout/exam-jee-346.jpg'),
                '--out', str(out)], capture_output=True, text=True)
            self.assertNotEqual(p.returncode, 0)
            report = json.loads((out / 'report.json').read_text())
            self.assertEqual(report['overall'], 'failed')
            self.assertEqual(report['checks']['runtime'], 'not_run')

    def test_invalid_mask_is_rejected(self):
        boxes = np.array([[22, .9, 0, 0, 50, 50, 1]], dtype=np.float32)
        masks = np.full((1, 200, 200), 2, dtype=np.int32)
        with self.assertRaisesRegex(ValueError, 'mask_value_not_binary'):
            evaluate_candidates(boxes, masks, .5, (100, 100))

    @unittest.skipUnless(REAL_MODEL.is_file() and (ROOT.parent / 'MNN/build/GetMNNInfo').is_file(),
                         '需要真实模型与已编译 MNN')
    def test_reference_tolerance_failure_keeps_comparison(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder) / 'result'
            p = subprocess.run([sys.executable, str(ROOT / 'scripts/model_probe_layout.py'),
                '--image', str(ROOT / 'tests/fixtures/layout/exam-jee-346.jpg'),
                '--out', str(out), '--max-ref-abs', '0'], capture_output=True, text=True)
            self.assertNotEqual(p.returncode, 0)
            report = json.loads((out / 'report.json').read_text())
            self.assertEqual(report['checks']['reference_preprocess'], 'failed')
            self.assertTrue((out / 'preprocess-comparison.json').exists())

    @unittest.skipUnless(REAL_MODEL.is_file() and (ROOT.parent / 'MNN/build/GetMNNInfo').is_file(),
                         '需要真实模型与已编译 MNN')
    def test_other_image_has_no_automatic_human_reference(self):
        with tempfile.TemporaryDirectory() as folder:
            image = Path(folder) / 'other.png'
            pixels = np.zeros((20, 30, 3), dtype=np.uint8)
            pixels[:, 15:] = 255
            Image.fromarray(pixels).save(image)
            out = Path(folder) / 'result'
            subprocess.run([sys.executable, str(ROOT / 'scripts/model_probe_layout.py'),
                '--image', str(image), '--out', str(out), '--max-ref-abs', '0'],
                capture_output=True, text=True)
            report = json.loads((out / 'report.json').read_text())
            self.assertIsNone(report['sample']['human_reference'])
