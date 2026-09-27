"""固定整页评测的受控失败样例。"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from issue17_baseline import aggregate_quality_claim_blocked, corrected_annotation, score_page


def annotation():
    return {'layout_dets': [
        {'anno_id': 1, 'category_type': 'text_block', 'poly': [0, 0, 100, 0, 100, 30, 0, 30],
         'text': '正确正文 $x+1$', 'order': 1, 'ignore': False,
         'merge_list': [{'line_with_spans': [
             {'category_type': 'equation_inline', 'latex': '$x+1$',
              'poly': [40, 0, 70, 0, 70, 30, 40, 30]}]}]},
        {'anno_id': 2, 'category_type': 'text_block', 'poly': [0, 40, 100, 40, 100, 70, 0, 70],
         'text': '第二段', 'order': 2, 'ignore': False},
    ]}


def block(identifier, y, text='正确正文 $x+1$', status='ok'):
    return {'id': identifier, 'type': 'text', 'bbox': [0, y, 100, y + 30],
            'status': status, 'content': {'text': text},
            'provenance': {'raw_output': text}}


def document(blocks, status='ok'):
    return {'status': status, 'pages': [{'blocks': blocks,
            'reading_order': [b['id'] for b in blocks], 'relations': []}]}


class BaselineFailures(unittest.TestCase):
    def test_complete_control_is_not_blocked(self):
        report = score_page(annotation(), document([block('b1', 0), block('b2', 40, '第二段')]))
        self.assertFalse(report['quality_claim_blocked'])
        self.assertEqual(report['text']['exact'], 2)

    def test_missing_block_remains_in_denominator(self):
        report = score_page(annotation(), document([block('b1', 0)]))
        self.assertEqual(report['text']['denominator'], 2)
        self.assertEqual(report['text']['missing'], 1)
        self.assertTrue(report['quality_claim_blocked'])

    def test_duplicate_output_is_reported(self):
        report = score_page(annotation(), document([block('b1', 0), block('b2', 40, '第二段'),
                                                    block('duplicate', 0)]))
        self.assertEqual(report['text']['duplicate_outputs'], 1)
        self.assertEqual(report['text']['exact'], 2)
        self.assertTrue(report['quality_claim_blocked'])

    def test_full_fallback_cannot_score_as_correct(self):
        report = score_page(annotation(), document([block('b1', 0, status='failed'),
                                                     block('b2', 40, '第二段', 'failed')]))
        self.assertEqual(report['text']['exact'], 0)
        self.assertEqual(report['text']['fallback'], 2)
        self.assertEqual(report['inline_formula']['exact'], 0)
        self.assertTrue(report['quality_claim_blocked'])

    def test_errata_keeps_upstream_annotation_unchanged(self):
        upstream = annotation()
        upstream['layout_dets'][0]['text'] = '取 t=4时刻'
        spec = {'id': 'odb-15', 'image_sha256': 'fixed-image'}
        errata = {'items': [{'page_id': 'odb-15', 'annotation_id': 1,
                             'image_sha256': 'fixed-image', 'upstream_fragment': 't=4',
                             'corrected_fragment': 't=0'}]}
        revised = corrected_annotation(spec, upstream, errata)
        self.assertEqual(upstream['layout_dets'][0]['text'], '取 t=4时刻')
        self.assertEqual(revised['layout_dets'][0]['text'], '取 t=0时刻')

    def test_repeated_generation_is_visible(self):
        repeated = '重复生成的同一段内容' * 5
        report = score_page(annotation(), document([block('b1', 0, repeated),
                                                    block('b2', 40, '第二段')]))
        self.assertEqual(len(report['raw_repetition_suspicions']), 1)
        self.assertTrue(report['quality_claim_blocked'])

    def test_failed_document_gives_no_content_credit(self):
        report = score_page(annotation(), document([block('b1', 0),
                                                    block('b2', 40, '第二段')], 'failed'))
        self.assertEqual(report['text']['exact'], 0)
        self.assertEqual(report['text']['fallback'], 2)

    def test_nonzero_exit_blocks_aggregate_even_with_complete_document(self):
        complete = score_page(annotation(), document([block('b1', 0), block('b2', 40, '第二段')]))
        pages = {f'odb-{i:02d}': {'returncode': 0, 'scores': complete} for i in range(1, 21)}
        self.assertFalse(aggregate_quality_claim_blocked(pages))
        pages['odb-01']['returncode'] = 3
        self.assertTrue(aggregate_quality_claim_blocked(pages))


if __name__ == '__main__':
    unittest.main()
