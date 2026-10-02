"""在公共作业边界验证印刷公式归属、语义与可追溯导出。"""
from pathlib import Path
import sys
import tempfile

from cli_integration import png_2x2
from printed_page_integration import run


def main():
    with tempfile.TemporaryDirectory(prefix='dococr-formula-') as temporary:
        root = Path(temporary)
        image = root / 'page.png'
        image.write_bytes(png_2x2())
        output, document, manifest = run(sys.argv[1], 'printed_page_formula', root, image)
        page = document['pages'][0]
        blocks = page['blocks']
        assert [b['type'] for b in blocks] == ['text', 'formula', 'text']
        assert [b['status'] for b in blocks] == ['ok', 'ok', 'ok']
        assert blocks[0]['content']['text'] == '设$x^2+1$。'
        assert blocks[0]['bbox'] == [0, 0, 2, 1]
        assert blocks[1]['content']['format'] == 'latex'
        assert blocks[1]['content']['display'] is True
        assert blocks[1]['content']['text'] == r'\frac{a}{b}=c'
        assert blocks[1]['provenance']['raw_output'] == '$$\n\\frac{a}{b}=c\n$$'
        assert blocks[2]['content']['text'] == '1. 请计算'
        assert all((output / b['content']['resource']).exists() for b in blocks)
        assert len(page['layout_blocks']) == 4
        relation = page['relations'][0]
        assert relation['type'] == 'content_owned_by'
        assert relation['owner_block_id'] == blocks[0]['id']
        assert relation['source_layout_block_id'] in page['regions'][0]['source_layout_block_ids']
        assert document['layout_diagnostics']['candidates'][1]['handling_reason'] == (
            'inline_formula_owned_by_text')
        assert blocks[0]['provenance']['raw_output'] == blocks[0]['content']['text']
        markdown = (output / 'document.md').read_text()
        assert markdown.count('$x^2+1$') == 1
        assert markdown.count('$$') == 2
        assert markdown.count(r'\frac{a}{b}=c') == 1
        assert r'\tag{1}' not in markdown
        assert len(manifest['regions']) == 3
        for scenario, raw, error in [
            ('printed_page_formula_unclosed', r'$$\frac{a}{b}=c', 'invalid_formula_syntax'),
            ('printed_page_formula_prose', 'Please solve x+y', 'invalid_formula_syntax'),
            ('printed_page_formula_unknown_command', r'$$\foo{a}$$', 'invalid_formula_syntax'),
            ('printed_page_formula_missing_arg', r'$$\frac{a}$$', 'invalid_formula_syntax'),
            ('printed_page_formula_empty_arg', r'$$\frac{}{b}=c$$', 'invalid_formula_syntax'),
            ('printed_page_formula_sqrt_empty', r'$$\sqrt{}$$', 'invalid_formula_syntax'),
            ('printed_page_formula_spaced_empty', r'$$\frac {}{b}=c$$',
             'invalid_formula_syntax'),
            ('printed_page_formula_env_mismatch',
             r'$$\begin{aligned}x\end{matrix}$$', 'invalid_formula_syntax'),
            ('printed_page_formula_left_missing', r'$$\left\right$$', 'invalid_formula_syntax'),
            ('printed_page_formula_percent', '$$x%hidden$$', 'invalid_formula_syntax'),
            ('printed_page_formula_ampersand', '$$a&b$$', 'invalid_formula_syntax'),
            ('printed_page_formula_tagged', r'$$x=1\tag{1}$$', 'invalid_formula_syntax'),
            ('printed_page_formula_truncated', '$$\n\\frac{a}{b}=c\n$$', 'ovis_token_limit'),
        ]:
            failed_out, failed_doc, _ = run(sys.argv[1], scenario, root, image)
            formula = failed_doc['pages'][0]['blocks'][1]
            assert formula['status'] == 'partial' and formula['error'] == error, (
                scenario, formula['status'], formula['error'])
            assert formula['provenance']['raw_output'] == raw
            assert formula['content']['resource']
            assert (failed_out / formula['content']['resource']).exists()
            marker = '识别不完整' if scenario == 'printed_page_formula_truncated' else '待核验'
            assert f'[{marker}：b0002]' in (failed_out / 'document.md').read_text()
        mixed_out, mixed_doc, _ = run(sys.argv[1], 'printed_page_formula_mixed', root, image)
        mixed = mixed_doc['pages'][0]['blocks'][1]
        assert mixed_doc['schema_version'] == '1.11'
        assert mixed['type'] == 'formula' and mixed['status'] == 'ok'
        assert mixed['content']['format'] == 'markdown'
        assert mixed['content']['text'] == '$a+b$，则'
        assert '$a+b$，则' in (mixed_out / 'document.md').read_text()
        inline_out, inline_doc, _ = run(sys.argv[1], 'printed_page_formula_inline_wrapper', root, image)
        inline = inline_doc['pages'][0]['blocks'][1]
        assert inline['content']['display'] is False
        assert inline['content']['text'] == 'x^2+1'
        assert (inline_out / 'document.md').read_text().count('$x^2+1$') == 2
        compare_out, compare_doc, _ = run(sys.argv[1], 'printed_page_formula_comparison', root, image)
        compare = compare_doc['pages'][0]['blocks'][1]
        assert compare['status'] == 'ok'
        assert compare['content']['text'] == 'x>0'
        assert '$$\nx>0\n$$' in (compare_out / 'document.md').read_text()
        _, chinese_doc, _ = run(sys.argv[1], 'printed_page_formula_chinese_text', root, image)
        chinese = chinese_doc['pages'][0]['blocks'][1]
        assert chinese['status'] == 'ok'
        assert chinese['content']['text'] == r'\frac{\text{甲}}{b}=c'
        # 普通圆/方括号属于显示符号。数学写法是否合理不作为 LaTeX 错误。
        for scenario, expected in [
            ('printed_page_formula_fragment', 'a+(b'),
            ('printed_page_formula_misnested', '([)]'),
            ('printed_page_formula_misnested_brace', '({)}'),
        ]:
            rendered_out, rendered_doc, _ = run(sys.argv[1], scenario, root, image)
            rendered = rendered_doc['pages'][0]['blocks'][1]
            assert rendered['status'] == 'ok' and rendered['error'] is None
            assert rendered['content']['text'] == expected
            assert expected in (rendered_out / 'document.md').read_text()
        for scenario, expected in [
            ('printed_page_formula_scalable_dot', r'\left. x\right)'),
            ('printed_page_formula_scalable_angle', r'\left\langle x\right\rangle'),
        ]:
            _, display_doc, _ = run(sys.argv[1], scenario, root, image)
            display = display_doc['pages'][0]['blocks'][1]
            assert display['status'] == 'ok'
            assert display['content']['text'] == expected
        for scenario, parent_raw in [
            ('printed_page_formula_parent_unclosed', '设$x^2+1。'),
            ('printed_page_formula_parent_missing_arg', '设$\\frac{a}$，请计算。'),
        ]:
            parent_out, parent_doc, _ = run(sys.argv[1], scenario, root, image)
            parent = parent_doc['pages'][0]['blocks'][0]
            assert parent['status'] == 'partial'
            assert parent['error'] == 'invalid_inline_formula_syntax'
            assert parent['content']['text'] == parent_raw
            assert parent['provenance']['raw_output'] == parent_raw
            assert (parent_out / parent['content']['resource']).exists()
            assert '[待核验：b0001]' in (parent_out / 'document.md').read_text()
        _, single_doc, _ = run(sys.argv[1], 'printed_page_formula_parent_single_symbol', root, image)
        assert single_doc['pages'][0]['blocks'][0]['status'] == 'ok'
        assert single_doc['pages'][0]['blocks'][0]['content']['text'] == (
            '设$r$，可得$x^2+1$。')
        _, currency_doc, _ = run(sys.argv[1], 'printed_page_formula_parent_currency', root, image)
        currency = currency_doc['pages'][0]['blocks'][0]
        assert currency['status'] == 'ok'
        assert currency['content']['text'] == '价格 $5，设$x^2+1$。'
        _, numeric_doc, _ = run(sys.argv[1], 'printed_page_formula_parent_numeric', root, image)
        numeric = numeric_doc['pages'][0]['blocks'][0]
        assert numeric['status'] == 'ok'
        assert numeric['content']['text'] == '已知 $2 = a$，求$x^2+1$。'
        for scenario, expected in [
            ('printed_page_formula_parent_numeric_product', '已知 $2 x$，求$x^2+1$。'),
            ('printed_page_formula_parent_numeric_macro', '已知 $2 \\alpha$，求$x^2+1$。'),
        ]:
            _, numeric_doc, _ = run(sys.argv[1], scenario, root, image)
            numeric = numeric_doc['pages'][0]['blocks'][0]
            assert numeric['status'] == 'ok'
            assert numeric['content']['text'] == expected


if __name__ == '__main__':
    main()
