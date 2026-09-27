"""在公共 CLI 作业边界验证 PDF 原页序与稳定命名空间。"""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from PIL import Image
import jsonschema
from printed_page_integration import config, ROOT
from layout_integration import config as layout_config

PDF_SCHEMA = json.loads((ROOT / 'docs/issue-11/document-ir-1.4.schema.json').read_text())


def run(binary, setting, pdf, out, *options, expected=0, env=None):
    process = subprocess.run(
        [binary, '--config', str(setting), '--input', str(pdf),
         '--out', str(out), *options], cwd=ROOT, capture_output=True, text=True, env=env)
    assert process.returncode == expected, (process.returncode, process.stdout, process.stderr)
    if expected == 0:
        document = json.loads((out / 'document.json').read_text(encoding='utf-8'))
        jsonschema.validate(document, PDF_SCHEMA)
        return document
    return process


def main():
    with tempfile.TemporaryDirectory(prefix='dococr-pdf-') as temporary:
        root = Path(temporary)
        pdf = root / '中文 试卷.pdf'
        images = [Image.new('RGB', (100, 100), color) for color in
                  ('white', '#fefefe', '#fdfdfd')]
        images[0].save(pdf, save_all=True, append_images=images[1:])
        setting = root / 'config.json'
        setting.write_text(json.dumps(config('printed_page_reading')))
        tool_env = None
        if os.name != 'nt':
            tool_dir = root / '中文 Poppler 工具'
            tool_dir.mkdir()
            for name in ('pdfinfo', 'pdftoppm'):
                shutil.copy2(shutil.which(name), tool_dir / name)
            tool_env = dict(os.environ, DOCOCR_POPPLER_BIN=str(tool_dir))
        selected = run(sys.argv[1], setting, pdf, root / 'selected', '--pages', '2-3',
                       '--dpi', '72', env=tool_env)
        assert selected['schema_version'] == '1.4'
        assert [p['page_id'] for p in selected['pages']] == ['p0002', 'p0003']
        assert [p['pdf_page_number'] for p in selected['pages']] == [2, 3]
        ids = [block['id'] for page in selected['pages'] for block in page['blocks']]
        assert len(ids) == len(set(ids))
        resources = [asset['path'] for asset in selected['resources']]
        assert len(resources) == len(set(resources))
        assert all((root / 'selected' / name).is_file() for name in resources)
        whole = run(sys.argv[1], setting, pdf, root / 'whole', '--dpi', '72')
        assert selected['pages'] == whole['pages'][1:]
        assert selected['resources'] == [r for r in whole['resources']
                                         if r['source_block_id'].startswith(('p0002-', 'p0003-'))]
        manifest = json.loads((root / 'selected' / 'run-manifest.json').read_text())
        assert [p['page_id'] for p in manifest['pdf']['pages']] == ['p0002', 'p0003']
        assert all(isinstance(p['rss_before_bytes'], int) and p['rss_before_bytes'] > 0
                   for p in manifest['pdf']['pages'])
        assert manifest['pdf']['effective_max_page_pixels'] <= 16_000_000
        assert re.fullmatch(r'\d+\.\d+\.\d+', manifest['pdf']['renderer_version'])
        bad_range = run(sys.argv[1], setting, pdf, root / 'bad-range', '--pages', '4-4',
                        expected=3)
        assert 'pdf_invalid_page_range' in bad_range.stderr
        too_large = run(sys.argv[1], setting, pdf, root / 'too-large', '--pages', '1-1',
                        '--max-page-pixels', '100', expected=5)
        assert 'pdf_page_pixel_budget' in too_large.stderr
        budget_manifest = json.loads((root / 'too-large' / 'run-manifest.json').read_text())
        assert budget_manifest['pdf']['pages'][0]['error']['code'] == 'pdf_page_pixel_budget'
        huge = run(sys.argv[1], setting, ROOT / 'tests/fixtures/pdf/huge_page.pdf',
                   root / 'huge', '--dpi', '150', expected=5)
        assert 'pdf_page_pixel_budget' in huge.stderr
        huge_manifest = json.loads((root / 'huge' / 'run-manifest.json').read_text())
        assert huge_manifest['pdf']['pages'][0]['estimated_raster_pixels'] > 16_000_000
        assert huge_manifest['pdf']['pages'][0].get('raster_size') is None
        unconfigured_huge_path = root / 'unconfigured-huge'
        unconfigured_huge = subprocess.run(
            [sys.argv[1], '--backend', 'fixture:sample',
             '--input', str(ROOT / 'tests/fixtures/pdf/huge_page.pdf'),
             '--out', str(unconfigured_huge_path), '--dpi', '150'],
            cwd=ROOT, capture_output=True, text=True)
        assert unconfigured_huge.returncode == 5
        unconfigured_huge_manifest = json.loads(
            (unconfigured_huge_path / 'run-manifest.json').read_text())
        assert unconfigured_huge_manifest['pdf']['pages'][0]['error']['code'] == 'pdf_page_pixel_budget'
        invalid = root / 'damaged.pdf'
        invalid.write_bytes(b'%PDF-1.7\nnot a valid PDF')
        damaged = run(sys.argv[1], setting, invalid, root / 'damaged', expected=3)
        assert 'pdf_tool_failed' in damaged.stderr or 'pdf_invalid_metadata' in damaged.stderr
        encrypted = run(sys.argv[1], setting, ROOT / 'tests/fixtures/encrypted.pdf',
                        root / 'encrypted', expected=3)
        assert 'pdf_encrypted' in encrypted.stderr
        blank_setting = root / 'blank-config.json'
        blank_setting.write_text(json.dumps(layout_config('fixture:layout_empty')))
        blank_pdf = root / '清晰空白.pdf'
        Image.new('RGB', (2, 2), 'white').save(blank_pdf)
        blank = run(sys.argv[1], blank_setting, blank_pdf, root / 'blank', '--pages', '1-1',
                    '--dpi', '72')
        assert blank['status'] == blank['pages'][0]['status'] == 'blank'
        assert blank['pages'][0]['blocks'] == []
        budget_setting = root / 'output-budget-config.json'
        limited = config('printed_page_reading')
        limited['execution']['max_output_bytes'] = 10_000_000
        budget_setting.write_text(json.dumps(limited))
        budget = run(sys.argv[1], budget_setting, pdf, root / 'output-budget',
                     '--pages', '1-2', '--dpi', '72')
        assert budget['pages'][0]['status'] == 'partial'
        assert budget['pages'][1]['status'] == 'failed'
        assert budget['pages'][1]['error']['code'] == 'pdf_document_output_budget'
        budget_audit = json.loads((root / 'output-budget' / 'run-manifest.json').read_text())
        assert budget_audit['pdf']['pages'][1]['pipeline_audit']['did_layout'] is True
        assert budget_audit['pdf']['pages'][1]['pipeline_ms'] >= 0
        all_budget_setting = root / 'all-output-budget-config.json'
        all_limited = config('printed_page_reading')
        all_limited['execution']['max_output_bytes'] = 1
        all_budget_setting.write_text(json.dumps(all_limited))
        all_budget = run(sys.argv[1], all_budget_setting, pdf, root / 'all-output-budget',
                         '--pages', '1-1', '--dpi', '72', expected=5)
        all_audit = json.loads((root / 'all-output-budget' / 'run-manifest.json').read_text())
        assert all_audit['pdf']['pages'][0]['error']['code'] == 'pdf_page_output_budget'
        assert all_audit['failure_code'] == 'pdf_document_output_budget'
        assert 'pdf_document_output_budget' in all_budget.stderr, all_budget.stderr
        geometry = root / 'geometry'
        geometry_run = subprocess.run([sys.argv[1], '--backend', 'fixture:sample',
                                      '--input', str(ROOT / 'tests/fixtures/pdf/rotated_crop.pdf'),
                                      '--out', str(geometry), '--dpi', '72'],
                                      cwd=ROOT, capture_output=True, text=True)
        assert geometry_run.returncode == 0, geometry_run.stderr
        geometry_doc = json.loads((geometry / 'document.json').read_text())
        jsonschema.validate(geometry_doc, PDF_SCHEMA)
        geometry_page = geometry_doc['pages'][0]
        assert geometry_page['pdf_crop_box_points'] == [20, 30, 80, 150]
        assert geometry_page['pdf_rotation_degrees'] == 90
        assert geometry_page['pdf_page_size_points'] == [120, 60]
        assert geometry_page['pdf_points_to_raster_affine'] == [0, 1, 1, 0, -30, -20]
        assert geometry_page['raster_size'] == [120, 60]
        assert geometry_page['estimated_raster_pixels'] == 121 * 61
        mixed_pdf = root / '局部失败.pdf'
        mixed_images = [Image.new('RGB', (100, 100), color) for color in
                        ('white', 'black', '#e6e6e6')]
        mixed_images[0].save(mixed_pdf, save_all=True, append_images=mixed_images[1:])
        mixed_setting = root / 'mixed-config.json'
        mixed_setting.write_text(json.dumps(config('printed_page_reading_pdf_mixed')))
        mixed = run(sys.argv[1], mixed_setting, mixed_pdf, root / 'mixed', '--dpi', '72')
        assert [p['status'] for p in mixed['pages']] == ['partial', 'failed', 'blank']
        assert mixed['pages'][1]['error']['code'] == 'layout_inference_failed'
        assert mixed['pages'][1]['raster_size'] == [100, 100]
        assert mixed['pages'][2]['page_id'] == 'p0003'
        literals_setting = root / 'literals-config.json'
        literals_setting.write_text(json.dumps(config('printed_page_reading_pdf_literals')))
        literals = run(sys.argv[1], literals_setting, pdf, root / 'literals',
                       '--pages', '2-2', '--dpi', '72')
        raw = [b['provenance']['raw_output'] for b in literals['pages'][0]['blocks']]
        assert '字面 b0001 p0001 assets/p0001-b0001.png' in raw
        markdown = (root / 'literals' / 'document.md').read_text(encoding='utf-8')
        assert '字面 b0001 p0001 assets/p0001-b0001.png' in markdown


if __name__ == '__main__':
    main()
