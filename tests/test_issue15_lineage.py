"""Exercise lineage ownership even when duplicate content has a different box."""
import sys
from pathlib import Path
import unittest


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from issue15_lineage import lineage_failures  # noqa: E402


class Issue15LineageTest(unittest.TestCase):
    def page(self):
        return {'layout_blocks': [{'id': 'l0001'}, {'id': 'l0002'}],
                'regions': [
                    {'id': 'r0001', 'source_layout_block_ids': ['l0001', 'l0002']},
                ],
                'blocks': [{'id': 'b0001', 'source_region_ids': ['r0001']}],
                'relations': [{'type': 'content_owned_by',
                               'source_layout_block_id': 'l0002',
                               'owner_block_id': 'b0001'}]}

    def test_merged_content_has_one_owner_per_layout_source(self):
        self.assertEqual(lineage_failures(self.page()), [])

    def test_duplicate_source_is_rejected_despite_different_geometry(self):
        page = self.page()
        page['blocks'].append({'id': 'b0002', 'source_region_ids': ['r0001'],
                               'bbox': [50, 50, 70, 70], 'type': 'formula'})
        failures = lineage_failures(page)
        self.assertTrue(any('duplicate_content_lineage' in item for item in failures))
        self.assertTrue(any('owner_lineage_mismatch' in item for item in failures))

    def test_duplicate_relation_and_wrong_owner_are_rejected(self):
        page = self.page()
        page['relations'].append(page['relations'][0].copy())
        page['relations'][0]['owner_block_id'] = 'b9999'
        failures = lineage_failures(page)
        self.assertTrue(any('duplicate_ownership_relation' in item for item in failures))
        self.assertTrue(any('owner_lineage_mismatch' in item for item in failures))


if __name__ == '__main__':
    unittest.main()
