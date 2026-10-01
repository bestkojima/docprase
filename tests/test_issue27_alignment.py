"""公开评分命令：多 GT 共用父区域时逐字符记账，旧口径保留。"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AlignmentCommandTests(unittest.TestCase):
    def evaluate(self, texts, actual, status='ok', extra_blocks=(), formulas=(), other_gt=()):
        with tempfile.TemporaryDirectory(prefix='issue27-align-') as directory:
            root = Path(directory)
            annotation = dict(layout_dets=[dict(anno_id=i + 1, category_type='text_block',
                order=i + 1, poly=[0, i * 30, 100, i * 30, 100, (i + 1) * 30, 0, (i + 1) * 30],
                text=text) for i, text in enumerate(texts)])
            if formulas:
                annotation['layout_dets'][0]['line_with_spans'] = [
                    dict(category_type='equation_inline', latex=f) for f in formulas]
            annotation['layout_dets'].extend(other_gt)
            block = dict(id='b1', type='text', status=status, bbox=[0, 0, 100, 30 * len(texts)],
                source_region_ids=['r1'], content=dict(text=actual), provenance=dict(raw_output=actual))
            document = dict(status='partial' if status != 'ok' else 'ok', pages=[dict(
                blocks=[block, *extra_blocks], regions=[dict(id='r1', source_layout_block_ids=['l1'])],
                reading_order=['b1', *(b['id'] for b in extra_blocks)])])
            for name, value in [('annotation', annotation), ('document', document)]:
                (root / f'{name}.json').write_text(json.dumps(value))
            command = [sys.executable, str(ROOT / 'scripts/issue27_alignment.py'),
                '--annotation', str(root / 'annotation.json'), '--document', str(root / 'document.json'),
                '--out', str(root / 'report.json')]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads((root / 'report.json').read_text())

    def test_two_gt_share_one_region_without_reusing_characters(self):
        value = self.evaluate(['Alpha.', 'Beta.'], 'Alpha.Beta.')
        self.assertEqual(value['original']['text']['missing'], 1)
        self.assertEqual(value['supplementary']['text']['missing'], 0)
        self.assertEqual(value['supplementary']['text']['edit_distance'], 0)
        rows = value['supplementary']['text']['rows']
        self.assertEqual([r['actual_range'] for r in rows], [[0, 6], [6, 11]])
        self.assertEqual(value['supplementary']['text']['reference_chars'], 11)
        self.assertTrue(value['content_audit']['verified'])
        self.assertFalse(value['supplementary']['quality_claim_blocked'])

    def test_ambiguous_geometry_does_not_choose_the_better_transcription(self):
        extra = dict(id='b2', type='text', status='ok', bbox=[0, 0, 100, 30],
            source_region_ids=['r2'], content=dict(text='Correct.'), provenance=dict(raw_output='Correct.'))
        value = self.evaluate(['Correct.'], 'Wrong.', extra_blocks=[extra])
        self.assertFalse(value['content_audit']['verified'])
        row = value['supplementary']['text']['rows'][0]
        self.assertIsNone(row['block_id'])
        self.assertEqual(row['status'], 'ambiguous_geometry')
        self.assertEqual(row['candidate_block_ids'], ['b1', 'b2'])

    def test_unmatched_output_is_an_insertion_not_free_content(self):
        extra = dict(id='b2', type='text', status='ok', bbox=[200, 0, 300, 30],
            source_region_ids=['r2'], content=dict(text='EXTRA'), provenance=dict(raw_output='EXTRA'))
        value = self.evaluate(['Alpha.'], 'Alpha.', extra_blocks=[extra])
        self.assertEqual(value['supplementary']['text']['edit_distance'], 5)
        self.assertFalse(value['content_audit']['verified'])

    def test_one_formula_occurrence_cannot_satisfy_two_gt_formulas(self):
        value = self.evaluate(['$x+1$ and $x+1$'], '$x+1$', formulas=['x+1', 'x+1'])
        self.assertEqual(value['original']['inline_formula']['exact'], 2)
        self.assertEqual(value['supplementary']['inline_formula']['denominator'], 2)
        self.assertEqual(value['supplementary']['inline_formula']['exact'], 1)

    def test_inline_gt_math_wrappers_do_not_become_formula_characters(self):
        value = self.evaluate(['$x+1$'], '$x+1$', formulas=['$x+1$'])
        self.assertEqual(value['supplementary']['inline_formula']['exact'], 1)

    def test_internal_order_requires_actual_content_evidence(self):
        correct = self.evaluate(['Alpha.', 'Beta.'], 'Alpha.Beta.')
        reversed_output = self.evaluate(['Alpha.', 'Beta.'], 'Beta.Alpha.')
        self.assertEqual(correct['supplementary']['reading_order']['denominator'], 1)
        self.assertEqual(correct['supplementary']['reading_order']['correct'], 1)
        self.assertEqual(reversed_output['supplementary']['reading_order']['correct'], 0)
        self.assertFalse(reversed_output['content_audit']['verified'])

    def test_exact_text_does_not_hide_a_missing_independent_formula(self):
        formula_gt = dict(anno_id=2, category_type='equation_isolated', order=2,
            poly=[0, 50, 100, 50, 100, 80, 0, 80], latex='x=1')
        value = self.evaluate(['Alpha.'], 'Alpha.', other_gt=[formula_gt])
        self.assertFalse(value['content_audit']['verified'])
        self.assertEqual(value['supplementary']['independent_formula']['missing'], 1)

    def test_fallback_keeps_full_deletion_and_raw_diagnostic(self):
        value = self.evaluate(['Alpha.', 'Beta.'], 'Alpha.Beta.', status='partial')
        text = value['supplementary']['text']
        self.assertEqual(text['reference_chars'], 11)
        self.assertEqual(text['edit_distance'], 11)
        self.assertEqual(text['raw_edit_distance'], 0)
        self.assertEqual(text['fallback'], 2)
        self.assertFalse(value['content_audit']['verified'])

    def test_unexecuted_full_corpus_cannot_pass(self):
        with tempfile.TemporaryDirectory(prefix='issue27-not-run-') as directory:
            root = Path(directory)
            report = root / 'report.json'
            result = subprocess.run([sys.executable, str(ROOT / 'scripts/issue27_report.py'),
                '--run', str(root), '--out', str(report)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2, result.stderr)
            value = json.loads(report.read_text())
            self.assertFalse(value['quality_gate']['passed'])
            evaluation = value['groups']['evaluation']['aggregate']['original']
            self.assertEqual(evaluation['text']['denominator'], 182)
            self.assertEqual(evaluation['text']['reference_chars'], 11685)
            self.assertEqual(evaluation['text']['edit_distance'], 11685)
            self.assertEqual(evaluation['inline_formula']['denominator'], 83)
            self.assertEqual(evaluation['table']['cells'], 36)
            self.assertEqual(evaluation['reading_order']['denominator'], 1675)
            self.assertEqual(len(value['groups']['development']['pages']), 20)


if __name__ == '__main__':
    unittest.main()
