"""从公共作业 CLI 验证版面去重和候选追溯。"""
import json
import pathlib
import subprocess
import sys
import tempfile

from cli_integration import png_2x2
from layout_integration import config, run


def main():
    fixture, production = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix='dococr-issue19-') as tmp:
        root = pathlib.Path(tmp)
        image = root / 'page.png'
        image.write_bytes(png_2x2())
        out = root / 'job'
        result = run(fixture, config('fixture:layout_dedup'), image, out)
        assert result.returncode == 0, result.stderr
        document = json.loads((out / 'document.json').read_text())
        candidates = document['layout_diagnostics']['candidates']
        assert [c['candidate_id'] for c in candidates if c['selected']] == [0, 4, 6]
        assert [c['filter_reason'] for c in candidates] == [
            None, 'nms_same_class', 'nms_cross_class', 'large_page_image',
            None, 'below_score_threshold', None]
        assert candidates[1]['original_bbox'] == [0, 0, 1, 1]
        assert [b['candidate_id'] for b in document['pages'][0]['layout_blocks']] == [0, 6, 4]
        page = document['pages'][0]
        layout_ids = {b['id'] for b in page['layout_blocks']}
        assert all(set(r['source_layout_block_ids']) <= layout_ids for r in page['regions'])
        region_ids = {r['id'] for r in page['regions']}
        assert all(set(b['source_region_ids']) <= region_ids for b in page['blocks'])
        assert len(page['reading_order']) == len(page['blocks']) == 3
        assert all((out / b['content']['resource']).exists() for b in document['pages'][0]['blocks'])
        assert (out / 'document.md').exists()
        reexport = root / 'reexport'
        result = subprocess.run([production, '--reexport', str(out / 'document.json'),
                                 '--asset-root', str(out), '--out', str(reexport)],
                                capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        rebuilt = json.loads((reexport / 'document.json').read_text())
        assert rebuilt['layout_diagnostics'] == document['layout_diagnostics']
        assert (reexport / 'document.md').read_bytes() == (out / 'document.md').read_bytes()

        edge_out = root / 'edges'
        result = run(fixture, config('fixture:layout_dedup_edges'), image, edge_out)
        assert result.returncode == 0, result.stderr
        edges = json.loads((edge_out / 'document.json').read_text())
        assert [c['filter_reason'] for c in edges['layout_diagnostics']['candidates']] == [
            None, 'nms_same_class', None, None, 'nms_cross_class', None]


if __name__ == '__main__':
    main()
