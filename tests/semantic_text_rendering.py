"""公共 CLI：图例按普通文字展示，答案/解析分段，原始内容与重导出不变。"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

from markdown_it import MarkdownIt
from PIL import Image
from printed_page_integration import ROOT, config

CASES = [
    ('legend', 24, '## 电子信息制造业企业利润总额增速\n\n## 工业企业利润总额增速\n',
     '电子信息制造业企业利润总额增速\n\n工业企业利润总额增速\n', False),
    ('caption', 7, '## 图1 统计结果\n', '图1 统计结果\n', False),
    ('answer', 22, '## [答案]:A [解析]:\n', '**答案：** A\n\n**解析：**\n', False),
    ('answer-mislabelled-heading', 17, '## [答案]:B [解析]:\n', '**答案：** B\n\n**解析：**\n', False),
    ('separate-lines', 22, '## [答案]:A\n[解析]:第一行。\n第二行。\n',
     '**答案：** A\n\n**解析：** 第一行。\n第二行。\n', False),
    ('inline-answer', 22, 'D. 原有选项 [答案]: C [解析]:根据题意可得 $x=1$。\n',
     'D. 原有选项\n\n**答案：** C\n\n**解析：** 根据题意可得 $x=1$。\n', False),
    ('fullwidth-labels', 22, '## 【答案】：BD 【解析】：第一行。\n第二行。\n',
     '**答案：** BD\n\n**解析：** 第一行。\n第二行。\n', False),
    ('analysis-only', 22, '## [解析]:由 $x>0$ 可得。\n', '**解析：** 由 $x>0$ 可得。\n', False),
    ('true-heading', 17, '## 一、选择题\n', '## 一、选择题\n', True),
    ('document-title', 6, '# 数学试卷\n', '# 数学试卷\n', True),
    ('literal-hash', 24, 'C# 与 #1 系列，浓度 $x>0$。\n', 'C# 与 #1 系列，浓度 $x>0$。\n', False),
    ('math-markers', 22, r'公式 $\text{[答案]:A [解析]:B}$。' + '\n',
     r'公式 $\text{[答案]:A [解析]:B}$。' + '\n', False),
    ('empty-labels', 22, '## [答案]: [解析]:\n', '**答案：**\n\n**解析：**\n', False),
    ('fenced-caption', 24, '```text\n## 字面内容\n```\n', '```text\n## 字面内容\n```\n', False),
    ('inline-code', 22, '格式为 `[答案]:A [解析]:示例`。\n', r'格式为 `\[答案]:A \[解析]:示例`。' + '\n', False),
    ('quoted-markers', 22, '说明采用“[答案]:A [解析]:示例”的格式。\n',
     r'说明采用“\[答案]:A \[解析]:示例”的格式。' + '\n', False),
    ('literal-marker-reference', 22, '这里提及 [解析]: 字段名称。\n', r'这里提及 \[解析]: 字段名称。' + '\n', False),
    ('unsafe-analysis-content', 22, '## [答案]:A [解析]:<img src=x onerror=alert(1)> & $x<y$\n',
     '**答案：** A\n\n**解析：** &lt;img src=x onerror=alert(1)&gt; &amp; $x<y$\n', False),
]


def check(args, root, case):
    name, class_id, raw, expected, heading = case
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
                            cwd=ROOT, env=dict(os.environ, DOCOCR_TEST_STRUCTURE_PATH=str(trace)),
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    doc = json.loads((job / 'document.json').read_text())
    b = doc['pages'][0]['blocks'][0]
    assert b['status'] == 'ok' and b['provenance']['raw_output'] == raw and b['content']['text'] == raw
    md = (job / 'document.md').read_text()
    tokens = MarkdownIt().parse(md)
    assert any(t.type == 'heading_open' for t in tokens) == heading, (name, '误生成标题', md)
    assert md == expected + '\n', (name, md, expected)
    exported = folder / 'reexport'
    result = subprocess.run([args.production_cli, '--reexport', str(job / 'document.json'),
                             '--asset-root', str(job), '--out', str(exported)], cwd=ROOT, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert (exported / 'document.md').read_bytes() == (job / 'document.md').read_bytes()
    assert (exported / 'document.json').read_bytes() == (job / 'document.json').read_bytes()
    if name == 'answer':
        # 旧版文档仍使用原来的展示行为，避免重新导出静默改变历史结果。
        doc['schema_version'] = '1.9'
        doc.pop('export_policy')
        for page in doc['pages']:
            for layout in page['layout_blocks']:
                layout.pop('semantic_label')
            for block in page['blocks']:
                block.pop('block_order')
        saved = folder / 'historical-1.9.json'
        saved.write_text(json.dumps(doc))
        legacy = folder / 'historical-1.9'
        result = subprocess.run([args.production_cli, '--reexport', str(saved), '--asset-root', str(job),
                                 '--out', str(legacy)], cwd=ROOT, capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        assert (legacy / 'document.md').read_text() == r'## \[答案]:A \[解析]:' + '\n\n'
        assert (legacy / 'document.json').read_bytes() == saved.read_bytes()
    print(name + ': PASS', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fixture_cli')
    parser.add_argument('production_cli')
    parser.add_argument('--case', choices=[c[0] for c in CASES])
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='dococr-semantic-text-') as temporary:
        for case in CASES:
            if not args.case or args.case == case[0]:
                check(args, Path(temporary), case)


if __name__ == '__main__':
    main()
