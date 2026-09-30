"""已归属但轻微越出父框的内容必须进入公共作业识别裁图。"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from PIL import Image
from layout_integration import ROOT, run
from printed_page_integration import config


def main():
    fixture, production = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix='dococr-owned-crop-') as tmp:
        root = Path(tmp)
        source = Image.new('RGB', (100, 100), 'white')
        source.paste((255, 0, 0), (50, 20, 51, 25))
        image = root / 'page.png'
        source.save(image)
        out = root / 'job'
        result = run(fixture, config('printed_page_reading_near_owned'), image, out)
        assert result.returncode == 0, result.stderr
        doc = json.loads((out / 'document.json').read_text())
        page = doc['pages'][0]
        assert len(page['blocks']) == len(page['regions']) == 1
        owner = page['blocks'][0]
        assert owner['bbox'] == [10, 10, 51, 50]
        layouts = {l['id']: l for l in page['layout_blocks']}
        region = page['regions'][0]
        assert region['bbox'] == owner['bbox']
        assert layouts[region['source_layout_block_ids'][0]]['bbox'] == [10, 10, 50, 50]
        relation = next(r for r in page['relations'] if r['type'] == 'content_owned_by')
        assert relation['owner_block_id'] == owner['id']
        assert layouts[relation['source_layout_block_id']]['bbox'] == [43, 20, 51, 25]
        candidate = doc['layout_diagnostics']['candidates'][0]
        assert candidate['crop_bbox'] == [10, 10, 50, 50]
        assert candidate['recognition_crop_bbox'] == [10, 10, 51, 50]
        assert candidate['crop_expansion_reason'] == 'owned_content_union'
        with Image.open(out / owner['content']['resource']) as crop:
            assert crop.size == (41, 40)
            assert crop.getpixel((40, 12)) == (255, 0, 0)
        assert len(json.loads((out / 'run-manifest.json').read_text())['regions']) == 1
        result = subprocess.run([production, '--reexport', str(out / 'document.json'),
                                 '--asset-root', str(out), '--out', str(root / 'rebuilt')],
                                cwd=ROOT, capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        assert json.loads((root / 'rebuilt/document.json').read_text()) == doc


if __name__ == '__main__':
    main()
