"""从公共 DocumentIR 验证路由验收不能被遗漏或错误任务绕过。"""
import copy
import gzip
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from issue28_audit import validate_controlled_job


class PublicRoutingAcceptance(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.job = Path(self.temp.name)
        archive = ROOT / 'docs/issue-28/evidence/runs/all_labels/document.json.gz'
        self.document = json.loads(gzip.decompress(archive.read_bytes()))
        for block in self.document['pages'][0]['blocks']:
            if block['type'] == 'image':
                asset = self.job / block['content']['resource']
                asset.parent.mkdir(parents=True, exist_ok=True)
                asset.write_bytes((ROOT / 'tests/fixtures/rgb2x2.png').read_bytes())

    def validate(self, document):
        (self.job / 'document.json').write_text(json.dumps(document))
        return validate_controlled_job(self.job, 'all_labels')

    def test_all_25_classes_have_their_expected_public_outputs(self):
        self.assertIn('all_classes_and_image_without_ovis', self.validate(self.document))

    def test_missing_image_outputs_cannot_pass(self):
        page = self.document['pages'][0]
        page['blocks'] = [block for block in page['blocks'] if block['type'] != 'image']
        with self.assertRaisesRegex(ValueError, '输出缺失或重复'):
            self.validate(self.document)

    def test_wrong_region_task_cannot_pass(self):
        self.document['pages'][0]['regions'][0]['recognition_type'] = 'table'
        with self.assertRaisesRegex(ValueError, '识别任务与来源类别'):
            self.validate(self.document)

    def test_duplicate_output_cannot_pass(self):
        page = self.document['pages'][0]
        duplicate = copy.deepcopy(page['blocks'][0])
        duplicate['id'] = 'extra-output'
        page['blocks'].append(duplicate)
        with self.assertRaisesRegex(ValueError, '输出缺失或重复'):
            self.validate(self.document)


if __name__ == '__main__':
    unittest.main()
