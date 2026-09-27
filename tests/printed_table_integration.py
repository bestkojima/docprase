"""从公共作业结果检查整表归属、语义、资源和不完整输出。"""
import copy
import json
from pathlib import Path
import sys
import tempfile

from cli_integration import png_2x2
import jsonschema
from layout_integration import config as layout_config, run as layout_run
from printed_page_integration import run


def main():
    with tempfile.TemporaryDirectory(prefix='dococr-table-') as temporary:
        root = Path(temporary)
        image = root / 'page.png'
        image.write_bytes(png_2x2())
        out, doc, manifest = run(sys.argv[1], 'printed_page_table', root, image)
        page = doc['pages'][0]
        assert doc['schema_version'] == '1.2'
        assert [block['type'] for block in page['blocks']] == ['text', 'table']
        table = page['blocks'][1]
        assert table['status'] == 'ok'
        assert table['content']['format'] == 'html'
        assert table['content']['text'] == (
            '<table><tr><th colspan="2">项目</th></tr>'
            '<tr><td>甲</td><td>$x+1$</td></tr></table>')
        cells = table['content']['table']['cells']
        assert [(c['row'], c['column'], c['rowspan'], c['colspan'], c['text']) for c in cells] == [
            (0, 0, 1, 2, '项目'), (1, 0, 1, 1, '甲'), (1, 1, 1, 1, '$x+1$')]
        assert all(c['bbox'] is None for c in cells)
        assert table['provenance']['raw_output'].startswith('<table border=1>')
        assert len(page['relations']) == 2
        assert all(r['owner_block_id'] == table['id'] for r in page['relations'])
        assert all(r['source_layout_block_id'] in page['regions'][1]['source_layout_block_ids']
                   for r in page['relations'])
        assert all(c['selected'] and c['mask_asset'] for c in
                   doc['layout_diagnostics']['candidates'])
        assert (out / table['content']['resource']).read_bytes().startswith(b'\x89PNG')
        markdown = (out / 'document.md').read_text()
        assert markdown.count('<table>') == 1
        assert markdown.count('$x+1$') == 1
        assert '外部表题' in markdown
        assert len(manifest['regions']) == 2
        schema = json.loads((Path(__file__).resolve().parents[1] /
                             'docs/issue-9/document-ir-1.2.schema.json').read_text())
        for status in ('partial', 'failed', 'skipped'):
            invalid = copy.deepcopy(doc)
            invalid_table = invalid['pages'][0]['blocks'][1]
            invalid_table['status'] = status
            invalid_table['content']['format'] = 'markdown'
            invalid_table['content']['text'] = ''
            try:
                jsonschema.validate(invalid, schema)
                assert False, (status, 'non-null table accepted')
            except jsonschema.ValidationError:
                pass
        _, merged, _ = run(sys.argv[1], 'printed_page_table_rowspan', root, image)
        merged_table = next(b for b in merged['pages'][0]['blocks'] if b['type'] == 'table')
        assert merged_table['status'] == 'ok'
        assert merged_table['content']['table']['rows'] == 2
        assert merged_table['content']['table']['columns'] == 2
        assert [(c['row'], c['column'], c['rowspan'], c['colspan'], c['text'])
                for c in merged_table['content']['table']['cells']] == [
            (0, 0, 2, 1, '甲'), (0, 1, 1, 1, '1'), (1, 1, 1, 1, '2')]
        layout_out = root / 'layout-only'
        process = layout_run(sys.argv[1], layout_config('fixture:layout_table'), image, layout_out)
        assert process.returncode == 0, process.stderr
        layout_doc = json.loads((layout_out / 'document.json').read_text())
        assert layout_doc['schema_version'] == '1.0'
        assert [b['type'] for b in layout_doc['pages'][0]['blocks']] == ['table', 'text']
        assert layout_doc['pages'][0]['relations'] == []
        for scenario, error in [
            ('printed_page_table_unclosed', 'invalid_table_structure'),
            ('printed_page_table_prefix', 'invalid_table_structure'),
            ('printed_page_table_truncated', 'ovis_token_limit'),
            ('printed_page_table_unsafe', 'invalid_table_structure'),
            ('printed_page_table_bad_span', 'invalid_table_structure'),
            ('printed_page_table_duplicate_span', 'invalid_table_structure'),
            ('printed_page_table_bad_section', 'invalid_table_structure'),
        ]:
            failed_out, failed, _ = run(sys.argv[1], scenario, root, image)
            block = next(b for b in failed['pages'][0]['blocks'] if b['type'] == 'table')
            assert block['status'] == 'partial' and block['error'] == error, scenario
            assert block['content']['format'] == 'markdown'
            assert block['content']['text'] == ''
            assert block['content']['table'] is None
            assert block['provenance']['raw_output']
            assert (failed_out / block['content']['resource']).exists()
            assert '<table' not in (failed_out / 'document.md').read_text()
        failed_out, failed_doc, _ = run(sys.argv[1], 'printed_page_table_failed', root, image)
        failed_table = next(b for b in failed_doc['pages'][0]['blocks'] if b['type'] == 'table')
        assert failed_table['status'] == 'failed'
        assert failed_table['content']['format'] == 'markdown'
        assert failed_table['content']['text'] == ''
        assert failed_table['content']['table'] is None
        assert failed_table['provenance']['raw_output'].startswith('<table')
        assert (failed_out / failed_table['content']['resource']).exists()
        _, skipped_doc, _ = run(sys.argv[1], 'printed_page_reset_failure', root, image)
        skipped_table = next(b for b in skipped_doc['pages'][0]['blocks'] if b['type'] == 'table')
        assert skipped_table['status'] == 'skipped'
        assert skipped_table['content']['format'] == 'markdown'
        assert skipped_table['content']['table'] is None


if __name__ == '__main__':
    main()
