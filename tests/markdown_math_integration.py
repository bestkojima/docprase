"""公共 CLI 导出：正文安全转义，数学原样输出，并可由真实 KaTeX 排版。"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

import jsonschema
from PIL import Image
from printed_page_integration import ROOT, config


def check_case(args, root, name, raw, expected, class_id=22, formulas=None, display_modes=None):
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
    jsonschema.validate(document, json.loads((ROOT / 'schemas/document-ir/document-ir-1.9-image.schema.json').read_text()))
    markdown = (job / 'document.md').read_text()
    assert markdown == expected + '\n', (name, markdown, expected)
    exported = folder / 'reexport'
    result = subprocess.run([args.production_cli, '--reexport', str(job / 'document.json'),
        '--asset-root', str(job), '--out', str(exported)], cwd=ROOT, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert (exported / 'document.json').read_bytes() == (job / 'document.json').read_bytes()
    assert (exported / 'document.md').read_bytes() == (job / 'document.md').read_bytes()
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


if __name__ == '__main__':
    main()
