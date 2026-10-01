"""从用户 CLI 边界核对保存的 DocumentIR 可以重新导出。"""
import json
import copy
import pathlib
import subprocess
import sys
import tempfile
from printed_page_integration import config
from PIL import Image
import jsonschema
from markdown_it import MarkdownIt
ROOT = pathlib.Path(__file__).resolve().parents[1]


def run(*args, code=0, cwd=ROOT):
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    assert result.returncode == code, (result.returncode, result.stdout, result.stderr)
    return result


def main():
    fixture, production, image = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="dococr-reexport-") as directory:
        root = pathlib.Path(directory)
        source, copied = root / "source", root / "copied"
        run(fixture, "--backend", "fixture:sample", "--input", image, "--out", str(source))
        run(production, "--reexport", str(source / "document.json"),
            "--asset-root", str(source), "--out", str(copied), cwd=root)
        assert (copied / "document.json").read_bytes() == (source / "document.json").read_bytes()
        assert (copied / "document.md").read_bytes() == (source / "document.md").read_bytes()
        document = json.loads((copied / "document.json").read_text())
        for resource in document["resources"]:
            path = resource["path"]
            assert (copied / path).read_bytes() == (source / path).read_bytes()

        attempts = 0
        def reexport(saved, value=None, code=0, asset_root=None):
            nonlocal attempts
            attempts += 1
            target = root / (saved.name + '-copy-' + str(attempts))
            input_json = saved / 'document.json'
            if value is not None:
                input_json = root / (saved.name + '-modified.json')
                input_json.write_text(json.dumps(value, ensure_ascii=False), encoding='utf-8')
            result = run(production, '--reexport', str(input_json), '--asset-root',
                         str(asset_root or saved), '--out', str(target), code=code, cwd=root)
            if code == 0:
                assert json.loads((target / 'document.json').read_text()) == json.loads(input_json.read_text())
                assert (target / 'document.md').read_bytes() == (saved / 'document.md').read_bytes()
                for path in collect_assets(value or json.loads(input_json.read_text())):
                    assert (target / path).read_bytes() == (saved / path).read_bytes(), path
            else:
                assert not target.exists()
            return result

        def generate(scenario):
            setting = root / (scenario + '.json')
            setting.write_text(json.dumps(config(scenario)), encoding='utf-8')
            saved = root / scenario
            input_image = image
            if scenario == 'printed_page_reading':
                input_image = root / 'large.png'
                Image.new('RGB', (100, 100), 'white').save(input_image)
            run(fixture, '--config', str(setting), '--input', str(input_image), '--out', str(saved))
            return saved, json.loads((saved / 'document.json').read_text())

        for name, schema_file in [('historical-1.1', 'schemas/document-ir/document-ir-1.1.schema.json'),
                                  ('historical-1.2', 'schemas/document-ir/document-ir-1.2.schema.json')]:
            saved = ROOT / 'tests' / 'fixtures' / 'reexport' / name
            data = json.loads((saved / 'document.json').read_text())
            jsonschema.validate(data, json.loads((ROOT / schema_file).read_text()))
            reexport(saved)
            if name.endswith('1.1'):
                assert [b['type'] for b in data['pages'][0]['blocks']] == ['formula', 'text']
                assert data['pages'][0]['relations'] == [{
                    'type': 'content_owned_by', 'source_layout_block_id': 'l0067',
                    'owner_block_id': 'b0006'}]
                assert data['pages'][0]['blocks'][0]['content']['text'].startswith(r'e^{\alpha}')
            else:
                table = next(b for b in data['pages'][0]['blocks'] if b['type'] == 'table')
                assert table['content']['table']['rows'] == 5
                assert table['content']['table']['columns'] == 3
                assert len(table['content']['table']['cells']) == 15

        generated = {}
        for scenario, version in [('formula_table', '1.0'),
                                  ('printed_page_formula', '1.9'),
                                  ('printed_page_table', '1.9'),
                                  ('printed_page_reading', '1.9'),
                                  ('printed_page_visual_empty', '1.9'),
                                  ('printed_page_failure', '1.9')]:
            if scenario == 'formula_table':
                saved = root / scenario
                run(fixture, '--backend', 'fixture:formula_table', '--input', image, '--out', str(saved))
                data = json.loads((saved / 'document.json').read_text())
            else:
                saved, data = generate(scenario)
            assert data['schema_version'] == version, (scenario, data['schema_version'])
            generated[scenario] = (saved, data)
            reexport(saved)
            assert [b['id'] for b in data['pages'][0]['blocks']] == data['pages'][0]['reading_order'] or scenario == 'printed_page_reading'
            assert all(b['confidence'] is None for b in data['pages'][0]['blocks'])

        table_saved, table = generated['printed_page_table']
        visual_saved, visual = generated['printed_page_visual_empty']
        invalid_visual = copy.deepcopy(visual)
        invalid_visual['pages'][0]['blocks'][0]['status'] = 'ok'
        assert '视觉' in reexport(visual_saved, invalid_visual, code=3).stderr
        invalid_visual = copy.deepcopy(visual)
        invalid_visual['pages'][0]['blocks'][3]['provenance'].pop('visual')
        assert '视觉' in reexport(visual_saved, invalid_visual, code=3).stderr
        formula_saved, formula = generated['printed_page_formula']
        def remove_new_crop_diagnostics(value):
            # 模拟历史1.1/1.2文件时，移除1.3之后引入的可选诊断字段。
            for candidate in value['layout_diagnostics']['candidates']:
                assert candidate.get('recognition_crop_bbox') is None
                candidate.pop('recognition_crop_bbox', None)
                candidate.pop('crop_expansion_reason', None)
        def remove_region_mapping(page):
            for region in page['regions']:
                region.pop('recognition_type', None)
            for block in page['blocks']:
                if block['type'] == 'image':
                    block['status'] = 'skipped'
                    block['error'] = 'recognition_not_executed'
                    if 'assessment' in block['provenance']:
                        block['provenance']['assessment'].update(
                            state='skipped', reason='recognition_not_executed')
        old_formula = copy.deepcopy(formula)
        old_formula['schema_version'] = '1.1'
        for old_page in old_formula['pages']:
            old_page.pop('structure_plan', None)
            remove_region_mapping(old_page)
            for relation in old_page['relations']:
                if relation.get('evidence') == 'pre_recognition_geometry':
                    relation['evidence'] = 'model_figure_title_and_geometry'
        remove_new_crop_diagnostics(old_formula)
        old_formula['pages'][0].pop('reading_order_evidence', None)
        for block in old_formula['pages'][0]['blocks']:
            block['reading_order_source'] = 'geometry'
            block['provenance'].pop('visual', None)
            block['provenance'].pop('assessment', None)
            block['provenance'].pop('recognition', None)
        jsonschema.validate(old_formula, json.loads((ROOT / 'schemas/document-ir/document-ir-1.1.schema.json').read_text()))
        reexport(formula_saved, old_formula)
        legacy = copy.deepcopy(table)
        legacy['schema_version'] = '1.2'
        for old_page in legacy['pages']:
            old_page.pop('structure_plan', None)
            remove_region_mapping(old_page)
            for relation in old_page['relations']:
                if relation.get('evidence') == 'pre_recognition_geometry':
                    relation['evidence'] = 'model_figure_title_and_geometry'
        remove_new_crop_diagnostics(legacy)
        legacy['pages'][0].pop('reading_order_evidence', None)
        for block in legacy['pages'][0]['blocks']:
            block['reading_order_source'] = 'geometry'
            block['provenance'].pop('visual', None)
            block['provenance'].pop('assessment', None)
            block['provenance'].pop('recognition', None)
        # 1.2 的表格语义与 1.3 相同；保存后的旧版文件仍须可读。
        jsonschema.validate(legacy, json.loads((ROOT / 'schemas/document-ir/document-ir-1.2.schema.json').read_text()))
        reexport(table_saved, legacy)
        for version, schema_path in [('1.3', 'schemas/document-ir/document-ir-1.3.schema.json'),
                                     ('1.5', 'schemas/document-ir/document-ir-1.5-image.schema.json'),
                                     ('1.7', 'schemas/document-ir/document-ir-1.7-image.schema.json')]:
            historical = copy.deepcopy(table)
            historical['schema_version'] = version
            for old_page in historical['pages']:
                old_page.pop('structure_plan', None)
                remove_region_mapping(old_page)
                for relation in old_page['relations']:
                    if relation.get('evidence') == 'pre_recognition_geometry':
                        relation['evidence'] = 'model_figure_title_and_geometry'
            for block in historical['pages'][0]['blocks']:
                if version != '1.7':
                    block['provenance'].pop('assessment')
                    block['provenance'].pop('recognition', None)
                if version == '1.3':
                    block['provenance'].pop('visual', None)
            jsonschema.validate(historical, json.loads((ROOT / schema_path).read_text()))
            reexport(table_saved, historical)

        without_optional_diagnostics = copy.deepcopy(table)
        historical_16 = copy.deepcopy(table)
        historical_16['schema_version'] = '1.6'
        for old_page in historical_16['pages']:
            old_page.pop('structure_plan', None)
            remove_region_mapping(old_page)
            for relation in old_page['relations']:
                if relation.get('evidence') == 'pre_recognition_geometry':
                    relation['evidence'] = 'model_figure_title_and_geometry'
        for block in historical_16['pages'][0]['blocks']:
            block['provenance'].pop('recognition')
        jsonschema.validate(historical_16, json.loads((ROOT / 'schemas/document-ir/document-ir-1.6-image.schema.json').read_text()))
        reexport(table_saved, historical_16)
        without_optional_diagnostics.pop('layout_diagnostics')
        for block in without_optional_diagnostics['pages'][0]['blocks']:
            block['provenance']['model_profile'] = 'MNN/3.0/PP-DocLayoutV3=fixture'
        reexport(table_saved, without_optional_diagnostics)

        current = copy.deepcopy(table)
        current['schema_version'] = '9.9'
        assert 'schema_version' in reexport(table_saved, current, code=3).stderr
        bad_schema = copy.deepcopy(document)
        bad_schema['unspecified_field'] = 1
        assert 'unspecified_field' in reexport(source, bad_schema, code=3).stderr
        bad_schema = copy.deepcopy(document)
        bad_schema['layout_diagnostics'] = None
        assert 'layout_diagnostics' in reexport(source, bad_schema, code=3).stderr
        bad_schema = copy.deepcopy(document)
        bad_schema['pages'][0]['blocks'][0]['reading_order_source'] = 'invented'
        assert 'reading_order_source' in reexport(source, bad_schema, code=3).stderr
        bad_schema = copy.deepcopy(table)
        bad_schema['pages'][0].pop('reading_order_evidence')
        assert 'reading_order_evidence' in reexport(table_saved, bad_schema, code=3).stderr
        untrusted = copy.deepcopy(document)
        untrusted['pages'][0]['blocks'][0]['content']['text'] = (
            '<img src=x onerror=alert(1)> ![外图](https://x)\n'
            '[点我](javascript:alert(1)) [遗失](missing.png) [引用][恶意] ![引用图][恶意]\n\n'
            '[恶意]: javascript:alert(1)\n\n'
            '[列表]\n\n- [列表]: https://example.com\n\n'
            '[编号]\n\n1. [编号]: https://example.com\n\n'
            '[引用块]\n\n> [引用块]: https://example.com\n\n'
            r'\[已转义](javascript:alert(1)) \\[双反斜杠](javascript:alert(1))')
        input_json = root / 'untrusted.json'
        input_json.write_text(json.dumps(untrusted, ensure_ascii=False))
        safe_out = root / 'safe-out'
        run(production, '--reexport', str(input_json), '--asset-root', str(source),
            '--out', str(safe_out), cwd=root)
        safe_markdown = (safe_out / 'document.md').read_text()
        assert '&lt;img src=x onerror=alert(1)&gt;' in safe_markdown
        assert r'\![外图]' in safe_markdown
        assert r'\[点我](javascript:alert(1))' in safe_markdown
        assert r'\[遗失](missing.png)' in safe_markdown
        assert r'\[引用][恶意]' in safe_markdown
        assert r'\[恶意]: javascript:alert(1)' in safe_markdown
        assert r'- \[列表]: https://example.com' in safe_markdown
        assert r'1. \[编号]: https://example.com' in safe_markdown
        assert r'&gt; \[引用块]: https://example.com' in safe_markdown
        assert r'\[已转义](javascript:alert(1))' in safe_markdown
        assert r'\\\[双反斜杠](javascript:alert(1))' in safe_markdown
        rendered = MarkdownIt().render(safe_markdown)
        assert '<a ' not in rendered, rendered
        assert 'src="https://x"' not in rendered, rendered
        assert 'src="assets/p0001-b0002.png"' in rendered
        assert json.loads((safe_out / 'document.json').read_text()) == untrusted
        old_table, _ = generated['formula_table']
        unsafe_table = json.loads((old_table / 'document.json').read_text())
        next(b for b in unsafe_table['pages'][0]['blocks'] if b['type'] == 'table')['content']['text'] = '<table><tr><td><script>x</script></td></tr></table>'
        assert '表格' in reexport(old_table, unsafe_table, code=3).stderr
        current = copy.deepcopy(table)
        current['pages'][0]['reading_order'][0] = 'b9999'
        assert 'reading_order' in reexport(table_saved, current, code=3).stderr
        current = copy.deepcopy(table)
        current['pages'][0]['relations'][0]['owner_block_id'] = 'b9999'
        assert '归属' in reexport(table_saved, current, code=3).stderr
        current = copy.deepcopy(table)
        current['pages'][0]['blocks'][0]['content']['resource'] = 'assets/absent.png'
        assert '原裁图资源不一致' in reexport(table_saved, current, code=3).stderr
        missing = root / 'missing-assets'
        missing.mkdir()
        assert '资源缺失' in reexport(table_saved, code=3, asset_root=missing).stderr
        current = copy.deepcopy(table)
        current['layout_diagnostics']['candidates'][0]['mask_asset'] = 'assets/missing-mask.png'
        assert '资源缺失' in reexport(table_saved, current, code=3).stderr
        current = copy.deepcopy(table)
        current['resources'][0]['path'] = '../outside.png'
        assert '.path' in reexport(table_saved, current, code=3).stderr

        pdf = root / 'mixed.pdf'
        pdf_images = [Image.new('RGB', (100, 100), color) for color in
                      ('white', 'black', '#e6e6e6')]
        pdf_images[0].save(pdf, save_all=True, append_images=pdf_images[1:])
        setting = root / 'pdf-config.json'
        setting.write_text(json.dumps(config('printed_page_reading_pdf_mixed')))
        pdf_saved = root / 'pdf-saved'
        run(fixture, '--config', str(setting), '--input', str(pdf), '--out', str(pdf_saved),
            '--dpi', '72')
        pdf_data = json.loads((pdf_saved / 'document.json').read_text())
        assert pdf_data['schema_version'] == '1.9'
        assert [p['status'] for p in pdf_data['pages']] == ['partial', 'failed', 'blank']
        reexport(pdf_saved)
        historical_pdf = copy.deepcopy(pdf_data)
        historical_pdf['schema_version'] = '1.4'
        for old_page in historical_pdf['pages']:
            old_page.pop('structure_plan', None)
            remove_region_mapping(old_page)
            for relation in old_page['relations']:
                if relation.get('evidence') == 'pre_recognition_geometry':
                    relation['evidence'] = 'model_figure_title_and_geometry'
        for page in historical_pdf['pages']:
            for block in page['blocks']:
                block['provenance'].pop('assessment')
                block['provenance'].pop('recognition', None)
                block['provenance'].pop('visual', None)
        jsonschema.validate(historical_pdf, json.loads((ROOT / 'schemas/document-ir/document-ir-1.4.schema.json').read_text()))
        reexport(pdf_saved, historical_pdf)
        visual_pdf = copy.deepcopy(pdf_data)
        visual_pdf['schema_version'] = '1.5'
        for old_page in visual_pdf['pages']:
            old_page.pop('structure_plan', None)
            remove_region_mapping(old_page)
            for relation in old_page['relations']:
                if relation.get('evidence') == 'pre_recognition_geometry':
                    relation['evidence'] = 'model_figure_title_and_geometry'
        for candidate in visual_pdf['pages'][0]['blocks']:
            candidate['provenance'].pop('assessment', None)
            candidate['provenance'].pop('recognition', None)
        block = next(b for b in visual_pdf['pages'][0]['blocks'] if b['status'] != 'ok')
        for candidate in visual_pdf['pages'][0]['blocks']:
            if candidate['status'] != 'ok' and candidate is not block:
                continue
            x0, y0, x1, y1 = candidate['bbox']
            width, height = x1-x0, y1-y0
            scale = min(256/width, 256/height)
            cw, ch = round(width*scale), round(height*scale)
            px, py = (256-cw)//2, (256-ch)//2
            sx, sy = width/cw, height/ch
            candidate['provenance']['visual'] = {
                'evidence': 'explicit_success' if candidate['status'] == 'ok' else 'no_visual_tokens',
                'token_count': 0, 'source_crop': candidate['content']['resource'],
                'source_bbox': candidate['bbox'], 'canvas_size': [256, 256],
                'content_size': [cw, ch], 'pad_offset': [px, py], 'scale': scale,
                'rounding_error': [cw-width*scale, ch-height*scale],
                'canvas_to_page_affine': [sx, 0, x0-px*sx, 0, sy, y0-py*sy],
                'stop_reason': 'normal' if candidate['status'] == 'ok' else 'vision_missing'}
        jsonschema.validate(visual_pdf, json.loads((ROOT / 'schemas/document-ir/document-ir-1.5-pdf.schema.json').read_text()))
        reexport(pdf_saved, visual_pdf)
        broken_pdf = copy.deepcopy(pdf_data)
        broken_pdf['pages'][0]['regions'][0]['page_id'] = 'p0003'
        assert 'region ID/page_id' in reexport(pdf_saved, broken_pdf, code=3).stderr
        broken_pdf = copy.deepcopy(pdf_data)
        broken_pdf['source']['selected_pages'] = [2, 4]
        assert 'selected_pages' in reexport(pdf_saved, broken_pdf, code=3).stderr
        broken_pdf = copy.deepcopy(pdf_data)
        broken_pdf['pages'][1]['page_index'] = 0
        assert 'page_id' in reexport(pdf_saved, broken_pdf, code=3).stderr
        broken_pdf = copy.deepcopy(pdf_data)
        broken_pdf['pages'][0]['pdf_points_to_raster_affine'] = [1, 2]
        assert 'affine' in reexport(pdf_saved, broken_pdf, code=3).stderr
        broken_pdf = copy.deepcopy(pdf_data)
        broken_pdf['pages'][1]['blocks'] = [copy.deepcopy(pdf_data['pages'][0]['blocks'][0])]
        assert '失败页' in reexport(pdf_saved, broken_pdf, code=3).stderr

        bad = copy.deepcopy(table)
        bad['pages'][0]['blocks'][0]['type'] = 'script'
        assert 'type' in reexport(table_saved, bad, code=3).stderr
        bad = copy.deepcopy(table)
        bad['pages'][0]['blocks'][0]['content']['format'] = 'script'
        assert 'format' in reexport(table_saved, bad, code=3).stderr
        bad = copy.deepcopy(table)
        bad['pages'][0]['blocks'][0]['source_region_ids'] = []
        assert 'source_region_ids' in reexport(table_saved, bad, code=3).stderr
        bad = copy.deepcopy(table)
        bad['resources'][0]['source_block_id'] = next(
            b['id'] for b in table['pages'][0]['blocks']
            if b['id'] != table['resources'][0]['source_block_id'])
        assert 'resource' in reexport(table_saved, bad, code=3).stderr
        bad = copy.deepcopy(table)
        bad['resources'][0]['bbox'] = [0, 0, 1, 1]
        assert 'bbox' in reexport(table_saved, bad, code=3).stderr
        bad = copy.deepcopy(table)
        bad['resources'][0]['path'] = 'assets/evil\x00name.png'
        assert '.path' in reexport(table_saved, bad, code=3).stderr
        bad = copy.deepcopy(table)
        next(b for b in bad['pages'][0]['blocks'] if b['type'] == 'table')['content']['table']['cells'][0]['text'] = '伪造'
        assert '表格单元格' in reexport(table_saved, bad, code=3).stderr
        duplicate = root / 'duplicate.json'
        duplicate.write_text((table_saved / 'document.json').read_text().replace(
            '"schema_version":"1.9"', '"schema_version":"1.9","schema_version":"1.9"', 1))
        target = root / 'duplicate-out'
        assert '重复' in run(production, '--reexport', str(duplicate), '--asset-root',
                            str(table_saved), '--out', str(target), code=3, cwd=root).stderr
        assert not target.exists()
        # 即使资源名在 JSON 中合法，输入根中的符号链接也不得穿出资源根。
        linked_root = root / 'linked-assets'
        linked_root.mkdir()
        (linked_root / 'assets').symlink_to(table_saved / 'assets', target_is_directory=True)
        assert '资源缺失或越界' in reexport(table_saved, code=3, asset_root=linked_root).stderr
        overlap = run(production, '--reexport', str(table_saved / 'document.json'),
                      '--asset-root', str(table_saved), '--out', str(table_saved / 'nested'), code=3,
                      cwd=root)
        assert '重叠' in overlap.stderr


def collect_assets(document):
    paths = {r['path'] for r in document['resources']}
    for holder in [document, *document['pages']]:
        d = holder.get('layout_diagnostics')
        if d:
            paths.add(d['overlay_asset'])
            paths.update(d['raw_tensor_assets'].values())
            paths.update(c['mask_asset'] for c in d['candidates'] if c['mask_asset'])
    return paths


if __name__ == "__main__":
    main()
