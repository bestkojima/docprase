"""公开父区域评分命令：完整分母、唯一归属、失败和历史口径。"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def ref(ident, text, bounds=(0,0,100,30), category='text_block'):
    x0,y0,x1,y1=bounds
    return dict(anno_id=ident, category_type=category, order=ident,
                poly=[x0,y0,x1,y0,x1,y1,x0,y1], text=text)


def block(ident, text, bounds=(0,0,100,60), kind='text', status='ok'):
    return dict(id=ident,type=kind,status=status,bbox=list(bounds),source_region_ids=['r'+ident],
                content=dict(text=text),provenance=dict(raw_output=text))


class ParentEvaluationTests(unittest.TestCase):
    def evaluate(self, refs, blocks, status='ok'):
        with tempfile.TemporaryDirectory(prefix='issue31-evaluation-') as directory:
            root=Path(directory)
            annotation=dict(layout_dets=refs)
            document=dict(status=status,pages=[dict(blocks=blocks,regions=[],reading_order=[b['id'] for b in blocks])])
            for name,value in [('annotation',annotation),('document',document)]:
                (root/f'{name}.json').write_text(json.dumps(value))
            result=subprocess.run([sys.executable,str(ROOT/'scripts/issue31_evaluation.py'),
                '--annotation',str(root/'annotation.json'),'--document',str(root/'document.json'),
                '--out',str(root/'report.json')],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            return json.loads((root/'report.json').read_text())

    def test_owned_table_text_cannot_be_output_twice(self):
        table=ref(1,'',(0,0,100,60),'table')
        table['html']='<table><tr><td>Alpha</td></tr></table>'
        child=ref(2,'Alpha',(5,5,90,25))
        predicted=block('table','',(0,0,100,60),'table')
        predicted['content']['table']=dict(rows=1,columns=1,cells=[dict(row=0,column=0,rowspan=1,colspan=1,header=False,text='Alpha')])
        value=self.evaluate([table,child],[predicted,block('duplicate','Alpha',(5,5,90,25))])
        self.assertEqual(value['parent']['text']['duplicate_outputs'],1)
        self.assertFalse(value['content_audit']['verified'])

    def test_parent_alignment_retains_original_denominator_and_old_score(self):
        value=self.evaluate([ref(1,'Alpha.'),ref(2,'Beta.',(0,30,100,60))],
                            [block('b1','Alpha.Beta.')])
        self.assertEqual(value['original']['text']['missing'],1)
        self.assertEqual(value['parent']['text']['missing'],0)
        self.assertEqual(value['parent']['text']['reference_chars'],11)
        self.assertEqual(value['parent']['text']['edit_distance'],0)
        self.assertEqual([r['actual_range'] for r in value['parent']['text']['rows']],[[0,6],[6,11]])
        self.assertEqual(len(value['metrics']),8)
        self.assertNotIn('inline_formula_exact_rate',value['metrics'])

        self.assertFalse(value['quality_passed'])  # 零公式/表格分母不可通过全集验收。

    def test_fallback_keeps_characters_and_raw_diagnostic(self):
        value=self.evaluate([ref(1,'Alpha.'),ref(2,'Beta.',(0,30,100,60))],
                            [block('b1','Alpha.Beta.',status='partial')],status='partial')
        self.assertEqual(value['parent']['text']['edit_distance'],11)
        self.assertEqual(value['parent']['text']['raw_edit_distance'],0)
        self.assertEqual(value['parent']['text']['fallback'],2)
        self.assertFalse(value['content_audit']['verified'])

    def test_empty_execution_retains_all_reference_characters(self):
        value=self.evaluate([ref(1,'Alpha.')],[],status='failed')
        self.assertEqual(value['parent']['text']['missing'],1)
        self.assertEqual(value['parent']['text']['edit_distance'],6)
        self.assertFalse(value['quality_passed'])

    def test_owned_formula_reference_is_retained_in_parent_and_history(self):
        parent=ref(1,'Value $x=1$.',(0,0,100,60))
        child=ref(2,'',(10,10,70,25),'equation_isolated');child['latex']='x=1'
        value=self.evaluate([parent,child],[block('b1','Value $x=1$.')])
        self.assertEqual(value['original']['independent_formula']['denominator'],1)
        self.assertEqual(value['parent']['independent_formula']['denominator'],0)
        self.assertEqual(value['parent']['text']['reference_chars'],12)
        self.assertEqual(value['reference_plan']['owned_references'][0]['reference'],child)

    def test_parent_without_child_content_cannot_remove_reference(self):
        parent=ref(1,'Value.',(0,0,100,60))
        child=ref(2,'',(10,10,70,25),'equation_isolated');child['latex']='x=1'
        value=self.evaluate([parent,child],[block('b1','Value.')])
        self.assertEqual(value['parent']['independent_formula']['denominator'],1)
        self.assertEqual(value['parent']['independent_formula']['missing'],1)
        self.assertTrue(value['reference_plan']['unresolved'])
        self.assertFalse(value['content_audit']['verified'])

    def test_one_parent_occurrence_cannot_cover_two_child_formulas(self):
        parent=ref(1,'Value $x=1$.',(0,0,100,60))
        first=ref(2,'',(10,10,70,25),'equation_isolated');first['latex']='x=1'
        second=ref(3,'',(10,30,70,45),'equation_isolated');second['latex']='x=1'
        value=self.evaluate([parent,first,second],[block('b1','Value $x=1$.')])
        self.assertEqual(value['parent']['independent_formula']['denominator'],1)
        self.assertTrue(value['reference_plan']['unresolved'])
        self.assertFalse(value['content_audit']['verified'])

    def test_inline_representation_difference_is_diagnostic_not_new_gate(self):
        parent=ref(1,'$x^2$')
        parent['line_with_spans']=[dict(category_type='equation_inline',latex='x^{2}')]
        value=self.evaluate([parent],[block('b1','$x^2$')])
        self.assertEqual(value['parent']['text']['edit_distance'],0)
        self.assertEqual(value['inline_formula_diagnostics'][0]['classification'],
                         'unreviewed_content_or_representation_difference')
        self.assertNotIn('inline_formula_exact_rate',value['metrics'])
        self.assertFalse(value['content_audit']['verified'])

    def test_reversed_parent_content_does_not_get_reference_order_credit(self):
        value=self.evaluate([ref(1,'Alpha.'),ref(2,'Beta.',(0,30,100,60))],
                            [block('b1','Beta.Alpha.')])
        self.assertEqual(value['parent']['reading_order']['correct'],0)
        self.assertFalse(value['content_audit']['verified'])


if __name__=='__main__':
    unittest.main()
