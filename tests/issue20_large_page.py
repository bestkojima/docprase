"""整页超过旧解码预算时，版面输入缩放后仍回映原页。"""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

from cli_integration import chunk
from layout_integration import config, run


def large_page_png():
    width, height = 4100, 4200
    row = b'\0' + b'\xff\xff\xff' * width
    return (b'\x89PNG\r\n\x1a\n' +
            chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(row * height)) + chunk(b'IEND', b''))


def main():
    fixture, production = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix='dococr-layout-large-') as tmp:
        root = Path(tmp)
        image = root / 'large.png'
        image.write_bytes(large_page_png())
        out = root / 'job'
        result = run(fixture, config('fixture:layout_smartresize'), image, out)
        assert result.returncode == 0, result.stderr
        doc = json.loads((out / 'document.json').read_text())
        import jsonschema
        schema = json.loads((Path(__file__).resolve().parents[1] /
                             'docs/issue-10/document-ir-1.3.schema.json').read_text())
        jsonschema.validate(doc, schema)
        assert doc['pages'][0]['raster_size'] == [4100, 4200]
        transform = doc['layout_diagnostics']['input_transform']
        assert transform['mode'] == 'smartresize_800'
        assert transform['source_size'] == [4100, 4200]
        assert transform['canvas_size'] == [800, 800]
        assert transform['content_size'] == [781, 800]
        assert transform['pad_offset'] == [9, 0]
        assert struct.unpack('<2f', (out / doc['layout_diagnostics']['raw_tensor_assets']
                                     ['scale_factor']).read_bytes()) == (1., 1.)
        block = doc['pages'][0]['layout_blocks'][0]
        assert block['bbox'] == [0, 0, 4100, 4200]
        assert doc['layout_diagnostics']['candidates'][0]['original_bbox'] == [0, 0, 4100, 4200]
        overlay = out / doc['layout_diagnostics']['overlay_asset']
        assert struct.unpack('>II', overlay.read_bytes()[16:24]) == (800, 800)
        assert (out / doc['pages'][0]['blocks'][0]['content']['resource']).is_file()
        rebuilt = root / 'rebuilt'
        result = subprocess.run([production, '--reexport', str(out / 'document.json'),
                                 '--asset-root', str(out), '--out', str(rebuilt)],
                                capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        assert json.loads((rebuilt / 'document.json').read_text()) == doc


if __name__ == '__main__':
    main()
