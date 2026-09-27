"""Checks that frozen business scoring counts missing pages and repeated anchors."""
import sys
from pathlib import Path
import unittest


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from issue15_quality import anchors_quality, pdf_quality  # noqa: E402


class Issue15QualityTest(unittest.TestCase):
    def test_anchor_positions_follow_reading_order_and_repeats_are_ambiguous(self):
        page = {'blocks': [
            {'id': 'late', 'provenance': {'raw_output': 'B B'}},
            {'id': 'early', 'provenance': {'raw_output': 'A'}},
        ], 'reading_order': ['early', 'late']}
        score = anchors_quality({'pages': [page]}, ['A', 'B'])
        self.assertEqual(score['anchors'][1]['locations'], [(0, 1, 0), (0, 1, 2)])
        self.assertFalse(score['anchors'][1]['unique'])
        self.assertEqual(score['pair_decidable'], 0)

    def test_missing_pdf_page_counts_every_reference_line_as_deleted(self):
        score = pdf_quality({'pages': []})
        self.assertEqual(score['reference_lines'], 10)
        self.assertEqual(len(score['lines']), 10)
        self.assertEqual(score['total_edit_distance'], score['total_reference_chars'])


if __name__ == '__main__':
    unittest.main()
