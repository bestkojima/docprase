"""整页超过旧解码预算时，版面输入缩放后仍回映原页。"""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from PIL import Image

from layout_integration import ROOT, config, run


def main():
    fixture, production = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix='dococr-layout-large-') as tmp:
        root = Path(tmp)
        image = root / 'large.png'
        source = Image.new('RGB', (4100, 4200), 'white')
        # 画布中心的单个 mask 像素覆盖原页约 (2052..2072, 2100..2121)。
        source.paste((255, 0, 0), (2050, 2100, 2075, 2125))
        source.save(image)
        # 超大页必须先获得显式预算；不能因配置为旧上限而放行。
        limited = config('fixture:layout_smartresize')
        limited['execution']['max_page_pixels'] = 16_000_000
        rejected = run(fixture, limited, image, root / 'limited')
        assert rejected.returncode == 5, (rejected.returncode, rejected.stderr)
        manifest = json.loads((root / 'limited' / 'run-manifest.json').read_text())
        assert manifest['budget_stage'] == 'input_pixels'
        out = root / 'job'
        result = run(fixture, config('fixture:layout_smartresize'), image, out)
        assert result.returncode == 0, result.stderr
        doc = json.loads((out / 'document.json').read_text())
        import jsonschema
        schema = json.loads((Path(__file__).resolve().parents[1] /
                             'docs/issue-26/document-ir-1.9-image.schema.json').read_text())
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
        crop_path = out / doc['pages'][0]['blocks'][0]['content']['resource']
        with Image.open(crop_path) as crop:
            assert crop.size == (4100, 4200)
            assert crop.getpixel((2060, 2110)) == (255, 0, 0)
            assert crop.getpixel((100, 100)) == (255, 255, 255)
        candidate = doc['layout_diagnostics']['candidates'][0]
        assert candidate['mask_row'] == 0 and candidate['mask_nonzero'] == 1
        with Image.open(out / candidate['mask_asset']) as mask:
            assert mask.size == (4100, 4200)
            assert mask.getpixel((2060, 2110)) == 255
            assert mask.getpixel((2020, 2110)) == 0
            assert mask.getpixel((100, 100)) == 0
        rebuilt = root / 'rebuilt'
        result = subprocess.run([production, '--reexport', str(out / 'document.json'),
                                 '--asset-root', str(out), '--out', str(rebuilt)],
                                cwd=ROOT, capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        assert json.loads((rebuilt / 'document.json').read_text()) == doc
        assert (rebuilt / 'document.md').read_bytes() == (out / 'document.md').read_bytes()

        # 同尺寸 PDF 栅格必须进入相同适配路径，且请求预算仍优先限制。
        pdf = root / 'large.pdf'
        source.save(pdf, resolution=72)
        setting = root / 'pdf-config.json'
        setting.write_text(json.dumps(config('fixture:layout_smartresize')))
        pdf_out = root / 'pdf-job'
        result = subprocess.run([fixture, '--config', str(setting), '--input', str(pdf),
                                 '--out', str(pdf_out), '--dpi', '72'],
                                cwd=ROOT, capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        pdf_doc = json.loads((pdf_out / 'document.json').read_text())
        pdf_schema = json.loads((Path(__file__).resolve().parents[1] /
                                 'docs/issue-26/document-ir-1.9-pdf.schema.json').read_text())
        jsonschema.validate(pdf_doc, pdf_schema)
        page = pdf_doc['pages'][0]
        assert page['raster_size'] == [4100, 4200]
        assert page['layout_diagnostics']['input_transform'] == transform
        assert page['layout_blocks'][0]['bbox'] == [0, 0, 4100, 4200]
        pdf_candidate = page['layout_diagnostics']['candidates'][0]
        with Image.open(pdf_out / pdf_candidate['mask_asset']) as mask:
            assert mask.size == (4100, 4200)
            assert mask.getpixel((2060, 2110)) == 255
            assert mask.getpixel((2020, 2110)) == 0
        pdf_crop_path = pdf_out / page['blocks'][0]['content']['resource']
        with Image.open(pdf_crop_path) as crop:
            assert crop.size == (4100, 4200)
            red, green, blue = crop.getpixel((2060, 2110))
            assert red > 200 and green < 50 and blue < 50
        result = subprocess.run([fixture, '--config', str(setting), '--input', str(pdf),
                                 '--out', str(root / 'pdf-limited'), '--dpi', '72',
                                 '--max-page-pixels', '16000000'], cwd=ROOT, capture_output=True, text=True)
        assert result.returncode == 5 and 'pdf_page_pixel_budget' in result.stderr
        budget = json.loads((root / 'pdf-limited' / 'run-manifest.json').read_text())
        assert budget['pdf']['pages'][0].get('raster_size') is None
        result = subprocess.run([production, '--reexport', str(pdf_out / 'document.json'),
                                 '--asset-root', str(pdf_out), '--out', str(root / 'pdf-rebuilt')],
                                cwd=ROOT, capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        assert json.loads((root / 'pdf-rebuilt' / 'document.json').read_text()) == pdf_doc
        assert (root / 'pdf-rebuilt' / 'document.md').read_bytes() == (pdf_out / 'document.md').read_bytes()


if __name__ == '__main__':
    main()
