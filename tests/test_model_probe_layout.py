"""在约定 model_probe CLI 边界检查可见状态及可追溯工件。"""
import gzip
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / 'scripts/model_probe_layout.py'
MODEL = ROOT / 'models/doclayout/PP-DocLayoutV3.mnn'
MNN = ROOT.parent / 'MNN'
EXAM = ROOT / 'tests/fixtures/layout/exam-jee-346.jpg'
NONSQUARE = MNN / 'test_img/1.png'
REAL_READY = MODEL.is_file() and (MNN / 'build/GetMNNInfo').is_file()


def invoke(out, *extra):
    return subprocess.run([sys.executable, str(ENTRY), '--out', str(out), *map(str, extra)],
                          cwd=ROOT, capture_output=True, text=True)


def read_report(out):
    return json.loads((out / 'report.json').read_text())


class LayoutProbeTests(unittest.TestCase):
    def test_missing_model_writes_failed_report(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder) / 'result'
            result = invoke(out, '--model', Path(folder) / 'missing.mnn')
            self.assertNotEqual(result.returncode, 0)
            report = read_report(out)
            self.assertEqual(report['overall'], 'failed')
            self.assertEqual(report['checks']['runtime'], 'not_run')
            self.assertIn('model_missing', report['errors'])

    @unittest.skipUnless(REAL_READY, '需要真实模型与已编译 MNN')
    def test_wrong_model_hash_is_rejected_with_report(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder) / 'result'
            result = invoke(out, '--model', ROOT / 'tests/fixtures/layout/reference-model-config.json')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('model_hash_mismatch', read_report(out)['errors'])

    @unittest.skipUnless(REAL_READY, '需要真实模型与已编译 MNN')
    def test_reference_hash_must_match_image(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder) / 'result'
            wrong = Path(folder) / 'wrong.json'
            wrong.write_text(json.dumps({'image_sha256': 'wrong', 'annotations': []}))
            result = invoke(out, '--reference', wrong)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('reference_sample_hash_mismatch', read_report(out)['errors'])

    @unittest.skipUnless(REAL_READY, '需要真实模型与已编译 MNN')
    def test_non_reference_resize_fails_and_keeps_difference(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder) / 'result'
            result = invoke(out, '--preprocess', 'opencv')
            self.assertNotEqual(result.returncode, 0)
            report = read_report(out)
            self.assertEqual(report['checks']['reference_preprocess'], 'failed')
            self.assertGreater(report['preprocess']['candidate_vs_reference']['max_abs'],
                               report['preprocess']['tolerance_max_abs'])
            self.assertTrue((out / 'preprocess-comparison.json').is_file())

    @unittest.skipUnless(REAL_READY, '需要真实模型与已编译 MNN')
    def test_exam_page_has_reproducible_candidates_and_masks(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder) / 'result'
            result = invoke(out)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = read_report(out)
            self.assertEqual(report['overall'], 'diagnostic_passed')
            self.assertEqual(report['checks']['reference_preprocess'], 'passed')
            self.assertEqual(report['checks']['reference_model_output'], 'not_verified')
            self.assertIsNotNone(report['sample']['human_reference'])
            rows = json.loads((out / 'candidates.json').read_text())
            self.assertEqual(len(rows), report['outputs']['candidate_count'])
            self.assertEqual([row['candidate_id'] for row in rows], list(range(len(rows))))
            self.assertEqual([row['mask_row'] for row in rows], list(range(len(rows))))
            self.assertGreater(report['outputs']['geometry']['rank_duplicate_count'], 0)
            self.assertFalse(report['outputs']['geometry']['rank_is_continuous'])
            self.assertEqual(report['outputs']['mask_reference_max_different_pixels'], 0)
            self.assertTrue((out / 'overlay.jpg').is_file())
            self.assertEqual(len(list((out / 'page-masks').glob('*.png'))), report['outputs']['selected_count'])
            with gzip.open(out / report['outputs']['raw_masks'][0], 'rb') as raw:
                masks = np.frombuffer(raw.read(), dtype='<i4')
            self.assertEqual(masks.size, len(rows) * 200 * 200)
            self.assertTrue(np.all((masks == 0) | (masks == 1)))

    @unittest.skipUnless(REAL_READY, '需要真实模型与已编译 MNN')
    def test_other_image_does_not_inherit_exam_reference(self):
        with tempfile.TemporaryDirectory() as folder:
            image = Path(folder) / 'other.png'
            pixels = np.zeros((20, 30, 3), dtype=np.uint8)
            pixels[:, 15:] = 255
            Image.fromarray(pixels).save(image)
            out = Path(folder) / 'result'
            result = invoke(out, '--image', image)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = read_report(out)
            self.assertIsNone(report['sample']['human_reference'])
            self.assertIsNone(report['outputs']['business_reference'])

    @unittest.skipUnless(REAL_READY and NONSQUARE.is_file(), '需要现有非方形技术样本')
    def test_nonsquare_page_records_outside_candidates_and_reference_mask_geometry(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder) / 'result'
            result = invoke(out, '--image', NONSQUARE)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = read_report(out)
            self.assertEqual(report['sample']['hw'], [1419, 2000])
            self.assertGreater(report['outputs']['rejected_by_reason']['outside_page'], 0)
            self.assertEqual(report['outputs']['mask_reference_max_different_pixels'], 0)
            self.assertIsNone(report['sample']['human_reference'])


if __name__ == '__main__':
    unittest.main()
