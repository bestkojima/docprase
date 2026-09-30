"""从公共 CLI 验证真实 Layout 契约下的转写结果、资源及局部失败。"""
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from cli_integration import png_2x2

ROOT = Path(__file__).resolve().parents[1]


def config(scenario):
    source = json.loads((ROOT / 'configs/printed-page.example.json').read_text())
    fixture = json.loads((ROOT / 'configs/fixture-plan.example.json').read_text())
    source['mode'] = 'development'
    source['backend'] = 'fixture:' + scenario
    source['models'] = copy.deepcopy(fixture['models'])
    source['execution'].update(layout_preprocess='auto', layout_score_threshold=.5)
    return source


def run(binary, scenario, root, image):
    setting = root / (scenario + '.json')
    setting.write_text(json.dumps(config(scenario)))
    output = root / scenario
    process = subprocess.run([binary, '--config', str(setting), '--input', str(image),
                              '--out', str(output)], cwd=ROOT, capture_output=True, text=True)
    assert process.returncode == 0, (process.returncode, process.stderr)
    document = json.loads((output / 'document.json').read_text())
    manifest = json.loads((output / 'run-manifest.json').read_text())
    import jsonschema
    schema_path = ('docs/issue-18/document-ir-1.5-image.schema.json'
                   if document['schema_version'] == '1.5'
                   else 'docs/issue-10/document-ir-1.3.schema.json'
                   if document['schema_version'] == '1.3'
                   else 'docs/issue-9/document-ir-1.2.schema.json'
                   if document['schema_version'] == '1.2'
                   else 'docs/issue-8/document-ir-1.1.schema.json'
                   if document['schema_version'] == '1.1'
                   else 'docs/issue-4/document-ir-1.0.schema.json')
    schema = json.loads((ROOT / schema_path).read_text())
    jsonschema.validate(document, schema)
    return output, document, manifest


def main():
    with tempfile.TemporaryDirectory(prefix='dococr-printed-page-') as temporary:
        root = Path(temporary)
        image = root / 'page.png'
        image.write_bytes(png_2x2())
        output, document, manifest = run(sys.argv[1], 'printed_page', root, image)
        page = document['pages'][0]
        blocks = page['blocks']
        assert document['schema_version'] == '1.5'
        assert blocks[0]['provenance']['visual']['evidence'] == 'explicit_success'
        assert blocks[0]['provenance']['visual']['token_count'] == 0
        assert blocks[0]['provenance']['visual']['canvas_size'] == [256, 256]
        assert [block['status'] for block in blocks] == [
            'ok', 'skipped', 'ok', 'ok', 'partial', 'skipped']
        assert [block['type'] for block in blocks] == [
            'text', 'image', 'table', 'text', 'formula', 'unknown']
        assert [block['bbox'] for block in blocks] == [[0, 0, 2, 1], [0, 0, 1, 1],
            [1, 0, 2, 1], [0, 1, 1, 2], [1, 1, 2, 2], [1, 1, 2, 2]]
        assert blocks[0]['content']['text'] == '中文，English!\n第二行。'
        assert blocks[0]['provenance']['raw_output'] == blocks[0]['content']['text']
        assert blocks[4]['content']['text'] == '## 公式=原始片段'
        assert blocks[4]['content']['format'] == 'markdown'
        assert blocks[4]['error'] == 'invalid_formula_syntax'
        assert blocks[2]['content']['text'] == '<table><tr><td>甲</td></tr></table>'
        assert blocks[2]['content']['format'] == 'html'
        assert blocks[1]['content']['resource'] and blocks[5]['content']['resource']
        assert all(block['confidence'] is None for block in blocks)
        assert all((output / block['content']['resource']).read_bytes().startswith(b'\x89PNG') for block in blocks)
        assert '中文，English!\n第二行。' in (output / 'document.md').read_text()
        assert [item['stop_reason'] for item in manifest['regions']] == [
            'normal', 'recognition_not_executed', 'normal', 'normal', 'normal', 'unknown_layout_class']
        assert all(item['elapsed_ms'] >= 0 for item in manifest['regions'])
        assert manifest['processing'][4]['status'] == 'delegated_runtime'
        markdown = (output / 'document.md').read_text()
        assert '![插图]' in markdown
        assert markdown.count('<table>') == 1 and '[待核验：b0005]' in markdown

        visual_out, visual_doc, visual_manifest = run(sys.argv[1], 'printed_page_visual_empty', root, image)
        visual_block = visual_doc['pages'][0]['blocks'][0]
        assert visual_doc['schema_version'] == '1.5'
        assert visual_block['status'] == 'failed'
        assert visual_block['provenance']['raw_output'] == '1. 2. spurious text'
        assert visual_block['provenance']['visual']['evidence'] == 'no_visual_tokens'
        assert visual_block['provenance']['visual']['token_count'] == 0
        assert visual_block['provenance']['visual']['source_bbox'] == visual_block['bbox']
        assert visual_block['provenance']['visual']['source_crop'] == visual_block['content']['resource']
        assert visual_block['provenance']['visual']['scale'] == 128
        assert visual_block['provenance']['visual']['rounding_error'] == [0, 0]
        assert visual_block['provenance']['visual']['canvas_to_page_affine'] == [1/128, 0, 0, 0, 1/128, -64/128]
        assert visual_manifest['regions'][0]['stop_reason'] == 'vision_missing'
        assert visual_doc['pages'][0]['blocks'][3]['status'] == 'ok'
        visual_markdown = (visual_out / 'document.md').read_text()
        assert '![原图](' in visual_markdown and '[识别失败：b0001]' in visual_markdown
        assert 'spurious text' not in visual_markdown

        missing_out, missing_doc, _ = run(sys.argv[1], 'printed_page_visual_missing_complete', root, image)
        missing_block = missing_doc['pages'][0]['blocks'][0]
        assert missing_block['status'] == 'failed' and missing_block['error'] == 'visual_evidence_missing'
        assert missing_block['content']['text'] == ''
        assert missing_block['provenance']['raw_output'] == 'spurious complete text'
        assert missing_doc['pages'][0]['blocks'][3]['status'] == 'ok'
        assert 'spurious complete text' not in (missing_out / 'document.md').read_text()

        for scenario, expected_status, expected_reason in [
            ('printed_page_failure', 'failed', 'error'),
            ('printed_page_empty', 'failed', 'empty_output'),
            ('printed_page_truncated', 'partial', 'token_limit'),
            ('printed_page_invalid_utf8', 'failed', 'error'),
            ('printed_page_reset_failure', 'failed', 'reset_failed'),
        ]:
            output, document, manifest = run(sys.argv[1], scenario, root, image)
            blocks = document['pages'][0]['blocks']
            assert blocks[0]['status'] == expected_status
            assert manifest['regions'][0]['stop_reason'] == expected_reason
            if scenario in ('printed_page_failure', 'printed_page_empty', 'printed_page_truncated'):
                assert blocks[3]['status'] == 'ok'
                assert blocks[3]['content']['text'] == '中文，English!\n第二行。'
            else:
                assert blocks[3]['status'] == 'skipped'
                assert blocks[3]['error'] == 'recognition_unavailable_after_backend_failure'
            assert blocks[0]['content']['resource']
            assert (output / blocks[0]['content']['resource']).exists()
            assert '[识别失败：b0001]' in (output / 'document.md').read_text() or scenario == 'printed_page_truncated'
            assert document['status'] == 'partial'
            if scenario == 'printed_page_failure':
                assert blocks[0]['provenance']['raw_output'] == 'partial raw'
            if scenario == 'printed_page_truncated':
                assert blocks[0]['content']['text'] == '截断'
            if scenario == 'printed_page_invalid_utf8':
                assert blocks[0]['error_base64'] == '/w=='

        _, filtered, filtered_manifest = run(sys.argv[1], 'printed_page_filtered', root, image)
        assert filtered['status'] == 'partial' and filtered['pages'][0]['blocks'] == []
        assert filtered['layout_diagnostics']['candidate_count'] == 1
        assert filtered_manifest['regions'] == []

        safe_out, safe_document, _ = run(sys.argv[1], 'printed_page_model_resource', root, image)
        original = ('正文<img src="https://example.invalid/x.png" />\n![](images/fake.png)'
                    '\n\\![单](https://example.invalid/one.png)'
                    '\n\\\\![双](https://example.invalid/two.png)'
                    '\n\\(x+1\\) \\[a+b\\]')
        assert safe_document['pages'][0]['blocks'][0]['content']['text'] == original
        assert safe_document['pages'][0]['blocks'][0]['provenance']['raw_output'] == original
        safe_markdown = (safe_out / 'document.md').read_text()
        assert '&lt;img src=' in safe_markdown
        assert r'\![]\(images/fake.png)' in safe_markdown
        assert '\\(x+1\\) \\[a+b\\]' in safe_markdown
        assert '<img src=' not in safe_markdown
        from markdown_it import MarkdownIt
        rendered = MarkdownIt().render(safe_markdown)
        assert 'src="https://example.invalid/' not in rendered
        assert 'src="images/fake.png"' not in rendered
        assert '<a href="https://example.invalid/' not in rendered
        assert '<a href="images/fake.png"' not in rendered

        legacy = json.loads((ROOT / 'configs/fixture-plan.example.json').read_text())
        legacy['backend'] = 'fixture:formula_table'
        legacy_cfg = root / 'legacy.json'
        legacy_cfg.write_text(json.dumps(legacy))
        legacy_out = root / 'legacy'
        process = subprocess.run([sys.argv[1], '--config', str(legacy_cfg), '--input',
            str(image), '--out', str(legacy_out)], cwd=ROOT, capture_output=True, text=True)
        assert process.returncode == 0, process.stderr
        assert '<table><tr><td>甲</td></tr></table>' in (legacy_out / 'document.md').read_text()


if __name__ == '__main__':
    main()
