"""公共 CLI 导出：正文安全转义，数学原样输出，并可由真实 KaTeX 排版。"""
import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile

import jsonschema
from PIL import Image
from printed_page_integration import ROOT, config


def check_case(args, root, name, raw, expected, class_id=22, formulas=None, display_modes=None,
               content_format=None):
    folder = root / name
    folder.mkdir()
    image = folder / 'page.png'
    Image.new('RGB', (600, 300), 'white').save(image)
    setting = config('printed_page_structure')
    setting['execution'].update(layout_preprocess='reference', layout_score_threshold=.3)
    cfg, trace, job = folder / 'config.json', folder / 'fixture.json', folder / 'job'
    cfg.write_text(json.dumps(setting))
    trace.write_text(json.dumps(dict(candidates=[[class_id, .95, 20, 20, 580, 200, 0]],
        outputs=[dict(bbox=[20, 20, 580, 200], text=raw)])))
    result = subprocess.run([args.fixture_cli, '--config', str(cfg), '--input', str(image), '--out', str(job)],
        env=dict(os.environ, DOCOCR_TEST_STRUCTURE_PATH=str(trace)), cwd=ROOT, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    document = json.loads((job / 'document.json').read_text())
    block = document['pages'][0]['blocks'][0]
    assert block['status'] == 'ok', (name, block['error'])
    assert block['provenance']['raw_output'] == raw
    if content_format:
        assert block['content']['format'] == content_format
        assert block['type'] == 'formula'
        assert len(document['pages'][0]['regions']) == 1
        assert len(block['provenance']['recognition']['attempts']) == 1
        assert block['content']['text'] == raw
    version = document['schema_version']
    jsonschema.validate(document, json.loads((ROOT / f'schemas/document-ir/document-ir-{version}-image.schema.json').read_text()))
    markdown = (job / 'document.md').read_text()
    assert markdown == expected + '\n', (name, markdown, expected)
    exported = folder / 'reexport'
    result = subprocess.run([args.production_cli, '--reexport', str(job / 'document.json'),
        '--asset-root', str(job), '--out', str(exported)], cwd=ROOT, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert (exported / 'document.json').read_bytes() == (job / 'document.json').read_bytes()
    assert (exported / 'document.md').read_bytes() == (job / 'document.md').read_bytes()
    if content_format:
        for field, value in [('text', '$x=999$'), ('format', 'latex'), ('display', True)]:
            altered = copy.deepcopy(document)
            altered['pages'][0]['blocks'][0]['content'][field] = value
            saved = folder / f'altered-{field}.json'
            saved.write_text(json.dumps(altered, ensure_ascii=False))
            rejected = subprocess.run([args.production_cli, '--reexport', str(saved),
                '--asset-root', str(job), '--out', str(folder / f'rejected-{field}')],
                cwd=ROOT, capture_output=True, text=True)
            assert rejected.returncode == 3, (name, field, rejected.stderr)
    print(f'{name}: PASS', flush=True)
    return dict(name=name, markdown=markdown, formulas=formulas or [],
        display_modes=display_modes or [False] * len(formulas or []))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fixture_cli')
    parser.add_argument('production_cli')
    parser.add_argument('--katex', metavar='NODE')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='dococr-markdown-math-') as temporary:
        root = Path(temporary)
        formula = r'M=\{x\mid\ln x<1\},\quad N=\{x\mid\frac{x+1}{x}<2\}'
        raw = '正文 <说明> & $' + formula + '$，且 $a>b$。'
        cases = [check_case(args, root, 'inline-comparisons', raw,
            '正文 &lt;说明&gt; &amp; $' + formula + '$，且 $a>b$。',
            formulas=[formula, 'a>b'])]
        formulas = [r'6\div 2=3', r'A\cup B', r'\epsilon=0.01', r'R=10\Omega']
        raw = '；'.join('$' + formula + '$' for formula in formulas) + '。'
        cases.append(check_case(args, root, 'common-math-symbols', raw, raw,
            formulas=formulas))
        raw = '价格 $5，公式 $x^2$；再付 $10。'
        cases.append(check_case(args, root, 'body-currency-with-math', raw,
            r'价格 \$5，公式 $x^2$；再付 \$10。', formulas=['x^2']))
        raw = '示例：`价格 $5 `，正文 $x^2$。'
        cases.append(check_case(args, root, 'currency-in-code-span', raw, raw,
            formulas=['x^2']))
        for name, raw, expected, formulas in [
            ('english', 'Price $5, formula $x^2$; pay $10.',
             r'Price \$5, formula $x^2$; pay \$10.', ['x^2']),
            ('decimal', '价格 $1,200.50，条件 $x>0$；余款 $0.25。',
             r'价格 \$1,200.50，条件 $x>0$；余款 \$0.25。', ['x>0']),
            ('multiple', '价格 $5 和 $10，公式 $x^2$ 与 $y^2$。',
             r'价格 \$5 和 \$10，公式 $x^2$ 与 $y^2$。', ['x^2', 'y^2']),
            ('escaped', r'价格 \$5，公式 $x^2$；再付 \$10。',
             r'价格 \$5，公式 $x^2$；再付 \$10。', ['x^2']),
            ('end', '求 $x^2$，费用 $5', r'求 $x^2$，费用 \$5', ['x^2']),
            ('mixed-escapes', r'价格 $5 和 \$10，公式 $x^2$。',
             r'价格 \$5 和 \$10，公式 $x^2$。', ['x^2']),
            ('adjacent-escaped-amount', r'价格 $5 \$10，公式 $x^2$。',
             r'价格 \$5 \$10，公式 $x^2$。', ['x^2']),
            ('numbers', '$2(x+1)$；$2[1+x]$；$2!$；$2FeO$；$2 x$；价格 $5。',
             r'$2(x+1)$；$2[1+x]$；$2!$；$2FeO$；$2 x$；价格 \$5。',
             ['2(x+1)', '2[1+x]', '2!', '2FeO', '2 x']),
            ('code-fence', '```text\n价格 $5 \n```\n\n$x^2$',
             '```text\n价格 $5 \n```\n\n$x^2$', ['x^2']),
        ]:
            cases.append(check_case(args, root, 'body-currency-' + name, raw, expected,
                formulas=formulas))
        raw = r'价格 $5，条件 \(x<y\)；结果 \[z=x+y\]。'
        expected = r'价格 \$5，条件 $x<y$；结果 $$z=x+y$$。'
        for class_id in [22, 5]:
            cases.append(check_case(args, root, f'currency-legacy-math-{class_id}', raw, expected,
                class_id=class_id, formulas=['x<y', 'z=x+y'], display_modes=[False, True],
                content_format='markdown' if class_id == 5 else None))
            for opening, closing, display in [(r'\(', r'\)', False), (r'\[', r'\]', True)]:
                adjacent_raw = '价格 $5 ' + opening + 'x^2' + closing + '。'
                delimiter = '$$' if display else '$'
                adjacent_expected = r'价格 \$5 ' + delimiter + 'x^2' + delimiter + '。'
                cases.append(check_case(args, root, f'currency-adjacent-legacy-{class_id}-{display}',
                    adjacent_raw, adjacent_expected, class_id=class_id, formulas=['x^2'], display_modes=[display],
                    content_format='markdown' if class_id == 5 else None))
        catalog = json.loads((ROOT / 'configs/formula-symbols.json').read_text())
        for category, names in catalog['categories'].items():
            formula = ' '.join('\\' + name for name in names)
            raw = '$' + formula + '$'
            cases.append(check_case(args, root, 'symbol-catalog-' + category, raw, raw,
                formulas=[formula]))
            cases.append(check_case(args, root, 'symbol-region-' + category, raw, raw,
                class_id=5, formulas=[formula]))
        raw = r'参数 $p_{sbl}$ 与 $q_{sik}$。'
        cases.append(check_case(args, root, 'long-variable-subscripts', raw, raw,
            formulas=['p_{sbl}', 'q_{sik}']))
        raw = r'$FeO$ 与 $NaOH$。'
        cases.append(check_case(args, root, 'chemical-symbol-sequences', raw, raw,
            formulas=['FeO', 'NaOH']))
        raw = r'方差 $s_{甲}^{2}=1.2$ 与 $s_{乙}^{2}=1.1$。'
        cases.append(check_case(args, root, 'chinese-script-labels', raw, raw,
            formulas=['s_{甲}^{2}=1.2', 's_{乙}^{2}=1.1']))
        first = r'\cos A\cos B=\frac{1}{2}[\cos(A+B)+\cos(A-B)]'
        second = r'\sin A\sin B=\frac{1}{2}[\cos(A-B)-\cos(A+B)]'
        raw = f'积化和差：${first}$；${second}$。'
        cases.append(check_case(args, root, 'formula-region-sequence', raw, raw,
            class_id=5, formulas=[first, second], content_format='markdown'))
        raw = '结论：$x<y$\n\n$$z=x+y$$。'
        cases.append(check_case(args, root, 'formula-region-mixed-modes', raw, raw,
            class_id=5, formulas=['x<y', 'z=x+y'], display_modes=[False, True], content_format='markdown'))
        raw = '<table><tr><td>$x^2$</td></tr></table>'
        cases.append(check_case(args, root, 'table-math', raw, raw,
            class_id=21, formulas=['x^2']))
        matrix = r'\begin{matrix}a&b\\c&d\end{matrix}'
        raw = ('<table><tr><th colspan="2">公式 &amp; 条件</th></tr>'
               '<tr><td rowspan="2">$a&lt;b$<br/>[原文](https://example.invalid)</td>'
               '<td>$$' + matrix.replace('&', '&amp;') + '$$</td></tr>'
               '<tr><td>&lt;img src=x onerror=alert(1)&gt;</td></tr></table>')
        cases.append(check_case(args, root, 'table-matrix-and-merged-cells', raw, raw,
            class_id=21, formulas=['a<b', matrix], display_modes=[False, True]))
        raw = r'<table><tr><td>条件 \(a&lt;b\)，值 \[x^2\]，$ x+1 $</td></tr></table>'
        cases.append(check_case(args, root, 'table-math-delimiters', raw, raw,
            class_id=21, formulas=['a<b', 'x^2', 'x+1'], display_modes=[False, True, False]))
        raw = '<table><tr><td>价格 $5，公式 $x^2$；再付 $10。</td></tr></table>'
        cases.append(check_case(args, root, 'table-currency-with-math', raw, raw,
            class_id=21, formulas=['x^2']))
        raw = '<table><tr><td>$2(x+1)$；$2[1+x]$；$2!$；$2FeO$；$2 x$</td></tr></table>'
        cases.append(check_case(args, root, 'table-number-leading-math', raw, raw,
            class_id=21, formulas=['2(x+1)', '2[1+x]', '2!', '2FeO', '2 x']))
        formula = r'[-1,+\infty)'
        raw = '区间 $' + formula + '$。'
        cases.append(check_case(args, root, 'half-open-interval-body', raw, raw,
            formulas=[formula]))
        formula = r'[a,b],\quad(a,b),\quad[a,b),\quad(a,b]'
        raw = '区间 $' + formula + '$。'
        cases.append(check_case(args, root, 'interval-endpoints', raw, raw, formulas=[formula]))
        formula = r'\left[-1,+\infty\right)'
        cases.append(check_case(args, root, 'scalable-half-open-interval', '$$' + formula + '$$',
            '$$\n' + formula + '\n$$', class_id=5, formulas=[formula], display_modes=[True]))
        formula = r'\left(a,\frac{1}{b}\right]'
        raw = '区间 $' + formula + '$。'
        cases.append(check_case(args, root, 'scalable-nested-interval', raw, raw, formulas=[formula]))
        formula = r'\left]a,b\right['
        raw = '区间 $' + formula + '$。'
        cases.append(check_case(args, root, 'scalable-open-interval', raw, raw, formulas=[formula]))
        formula = r'\begin{matrix}[a,b)&(a,b]\\{[c,d)}&{(c,d]}\end{matrix}'
        cases.append(check_case(args, root, 'interval-matrix', '$$' + formula + '$$',
            '$$\n' + formula + '\n$$', class_id=5, formulas=[formula], display_modes=[True]))
        matrix = r'\begin{matrix}a&b\\c&d\end{matrix}'
        cases.append(check_case(args, root, 'display-matrix', '$$' + matrix + '$$',
            '$$\n' + matrix + '\n$$', class_id=5, formulas=[matrix], display_modes=[True]))
        legacy = r'设 \(x<y\)，并有 \[x>0\]。'
        cases.append(check_case(args, root, 'legacy-delimiters', legacy,
            '设 $x<y$，并有 $$x>0$$。', formulas=['x<y', 'x>0'], display_modes=[False, True]))
        formula = r'A=\{x\mid -1<x\leqslant2\},\quad B=\{x\mid x^2-4x+3\leqslant0\}'
        raw = '$' + formula + '$'
        cases.append(check_case(args, root, 'chengdu-comparison', raw, raw, formulas=[formula]))
        piecewise = r'f(x)=\begin{cases}x^2&x<0\\x&x\ge0\end{cases}'
        cases.append(check_case(args, root, 'display-cases', '$$' + piecewise + '$$',
            '$$\n' + piecewise + '\n$$', class_id=5, formulas=[piecewise], display_modes=[True]))
        raw = '矩阵：$$' + matrix + '$$；求 $x>0$。'
        cases.append(check_case(args, root, 'parent-matrix', raw, raw,
            formulas=[matrix, 'x>0'], display_modes=[True, False]))
        raw = r'反应 $A(g)\rightleftharpoons P(g)$；填写 $z=$。'
        cases.append(check_case(args, root, 'chemical-arrow-and-answer-blank', raw, raw,
            formulas=[r'A(g)\rightleftharpoons P(g)', 'z=']))
        cases.append(check_case(args, root, 'independent-inline', '$x>0$', '$x>0$',
            class_id=5, formulas=['x>0']))
        raw = r'<img src=x onerror=alert(1)> & [公式 $x<y$](https://example.invalid)；价格 \$5。'
        cases.append(check_case(args, root, 'safe-prose-with-math', raw,
            r'&lt;img src=x onerror=alert(1)&gt; &amp; \[公式 $x<y$](https://example.invalid)；价格 \$5。',
            formulas=['x<y']))
        raw = '$ \n x<y \n $ 与 $$ \n x>0 \n $$'
        cases.append(check_case(args, root, 'math-whitespace', raw,
            '$x<y$ 与 $$x>0$$', formulas=['x<y', 'x>0'], display_modes=[False, True]))
        formula = r'\text{<img src=missing.png>}'
        cases.append(check_case(args, root, 'numbered-legacy-math',
            r'编号2\(' + formula + r'\)3', '编号2 $' + formula + '$ 3', formulas=[formula]))
        cases.append(check_case(args, root, 'numbered-dollar-math',
            '编号2$' + formula + '$3', '编号2 $' + formula + '$ 3', formulas=[formula]))
        cases.append(check_case(args, root, 'backslash-adjacent-math',
            r'行\\$' + formula + '$', r'行\\ $' + formula + '$', formulas=[formula]))
        formula += '\n+x'
        cases.append(check_case(args, root, 'numbered-display-multiline',
            r'编号2\[' + formula + r'\]3', '编号2 $$' + formula.replace('\n', ' ') + '$$ 3',
            formulas=[formula.replace('\n', ' ')], display_modes=[True]))
        formula = r'\text{<img src=missing.png>}'
        cases.append(check_case(args, root, 'numbered-dollar-display',
            '编号2$$' + formula + '$$3', '编号2 $$' + formula + '$$ 3',
            formulas=[formula], display_modes=[True]))
        formula += '\n+x'
        cases.append(check_case(args, root, 'backslash-adjacent-display',
            r'行\\$$' + formula + '$$', r'行\\ $$' + formula.replace('\n', ' ') + '$$',
            formulas=[formula.replace('\n', ' ')], display_modes=[True]))
        cases.append(check_case(args, root, 'multiline-legacy-inline',
            r'说明\(' + formula + r'\)。', '说明$' + formula.replace('\n', ' ') + '$。',
            formulas=[formula.replace('\n', ' ')]))
        cases.append(check_case(args, root, 'multiline-independent-inline',
            '$' + formula + '$', '$' + formula.replace('\n', ' ') + '$',
            class_id=5, formulas=[formula.replace('\n', ' ')]))
        formula = r'\text{<img src=missing.png>}' + '\r\n\r\n+x'
        cases.append(check_case(args, root, 'blank-line-parent-display',
            r'说明\[' + formula + r'\]。', '说明$$' + formula.replace('\r\n', ' ') + '$$。',
            formulas=[formula.replace('\r\n', ' ')], display_modes=[True]))
        if args.katex:
            manifest = root / 'cases.json'
            manifest.write_text(json.dumps(cases, ensure_ascii=False))
            subprocess.run([args.katex, str(ROOT / 'tests/math-rendering/render.mjs'), str(manifest)], check=True, cwd=ROOT)
            preview_source = root / 'table-matrix-and-merged-cells/job/document.md'
            subprocess.run([args.katex, str(ROOT / 'tests/math-rendering/preview.mjs'), str(preview_source)],
                check=True, cwd=ROOT)
            html = preview_source.with_suffix('.html').read_text()
            assert '<table>' in html and 'class="katex"' in html
            assert 'data:font/woff2;base64,' in html
            assert '$$' not in html and '<script' not in html


if __name__ == '__main__':
    main()
