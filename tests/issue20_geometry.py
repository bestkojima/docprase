"""通过公共作业输出检查包含关系、定位、顺序与重新导出。"""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

from cli_integration import chunk
from layout_integration import config, run


def page_png():
    row = b'\0' + b'\xff\xff\xff' * 100
    return (b'\x89PNG\r\n\x1a\n' +
            chunk(b'IHDR', struct.pack('>IIBBBBB', 100, 100, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(row * 100)) + chunk(b'IEND', b''))


def main():
    fixture, production = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix='dococr-issue20-') as tmp:
        root = Path(tmp)
        image = root / 'page.png'
        image.write_bytes(page_png())
        out = root / 'job'
        result = run(fixture, config('fixture:layout_geometry'), image, out)
        assert result.returncode == 0, result.stderr
        doc = json.loads((out / 'document.json').read_text())
        candidates = doc['layout_diagnostics']['candidates']
        assert candidates[1]['filter_reason'] == 'contained_by_large_class'
        assert candidates[3]['selected'] is True  # 行内公式不能被包含筛选误删
        assert candidates[5]['selected'] is True  # 表格内文字来源仍可追溯
        assert candidates[9]['filter_reason'] == 'outer_overlap'
        assert candidates[3]['mask_row'] == 3 and candidates[3]['mask_nonzero'] == 1
        assert candidates[8]['original_bbox'] == [-3, 88, 28, 103]
        assert candidates[8]['crop_bbox'] == [0, 88, 28, 100]
        actual_ids = [b['candidate_id'] for b in doc['pages'][0]['layout_blocks']]
        assert actual_ids == [2, 3, 4, 5, 8, 0, 6, 7], actual_ids
        assert len(doc['pages'][0]['reading_order']) == len(doc['pages'][0]['blocks'])
        assert all((out / b['content']['resource']).is_file()
                   for b in doc['pages'][0]['blocks'])
        rebuilt = root / 'rebuilt'
        result = subprocess.run([production, '--reexport', str(out / 'document.json'),
                                 '--asset-root', str(out), '--out', str(rebuilt)],
                                capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        assert json.loads((rebuilt / 'document.json').read_text()) == doc
        assert (rebuilt / 'document.md').read_bytes() == (out / 'document.md').read_bytes()


if __name__ == '__main__':
    main()
