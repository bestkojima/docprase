"""公共 CLI：合法数学正文可展示，首次导出与重新导出一致。"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

import jsonschema
from PIL import Image
from printed_page_integration import ROOT, config


def render_case(fixture_cli, production_cli, root, name, raw, class_id=22, valid=True,
                finish_reason='complete', stop_reason='normal', expected_error=None):
    folder = root / name
    folder.mkdir()
    image = folder / 'page.png'
    Image.new('RGB', (600, 300), 'white').save(image)
    setting = config('printed_page_structure')
    setting['execution'].update(layout_preprocess='reference', layout_score_threshold=.3)
    cfg, trace, job = folder / 'config.json', folder / 'fixture.json', folder / 'job'
    cfg.write_text(json.dumps(setting))
    trace.write_text(json.dumps(dict(candidates=[[class_id, .95, 20, 20, 580, 200, 0]],
        outputs=[dict(bbox=[20, 20, 580, 200], text=raw,
            finish_reason=finish_reason, stop_reason=stop_reason)])))
    result = subprocess.run([fixture_cli, '--config', str(cfg), '--input', str(image), '--out', str(job)],
        env=dict(os.environ, DOCOCR_TEST_STRUCTURE_PATH=str(trace)), cwd=ROOT, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    document = json.loads((job / 'document.json').read_text())
    block = document['pages'][0]['blocks'][0]
    assert block['type'] == ('formula' if class_id == 5 else 'text')
    assert block['provenance']['raw_output'] == raw
    assert block['status'] == ('ok' if valid else 'partial'), (name, block['status'], block['error'])
    markdown = (job / 'document.md').read_text()
    if valid:
        assert block['error'] is None
        assert block['content']['text'] == (raw[2:-2] if class_id == 5 else raw)
        assert '![原图]' not in markdown
        assert (raw if class_id == 22 else block['content']['text']) in markdown
    else:
        error = expected_error or ('invalid_formula_syntax' if class_id == 5 else 'invalid_inline_formula_syntax')
        assert block['error'] == error, (name, block['error'])
        marker = '识别不完整' if finish_reason == 'truncated' else '待核验'
        assert '![原图]' in markdown and f'[{marker}：' in markdown
        assert raw not in markdown
    assert (job / block['content']['resource']).is_file()
    jsonschema.validate(document, json.loads((ROOT / 'schemas/document-ir/document-ir-1.10-image.schema.json').read_text()))
    exported = folder / 'reexport'
    result = subprocess.run([production_cli, '--reexport', str(job / 'document.json'),
        '--asset-root', str(job), '--out', str(exported)], cwd=ROOT, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert (exported / 'document.md').read_bytes() == (job / 'document.md').read_bytes()
    print(f'{name}: PASS', flush=True)


def main():
    fixture_cli, production_cli = sys.argv[1:3]
    with tempfile.TemporaryDirectory(prefix='issue27-rendering-') as temporary:
        root = Path(temporary)
        render_case(fixture_cli, production_cli, root, 'chemical-arrow',
            r'反应 $A(g) \rightleftharpoons P(g)$，求平衡常数。')
        render_case(fixture_cli, production_cli, root, 'answer-blank',
            '满足 $(1+i)z=3+i$ 的复数 $z=$\n\nA. $2-i$\n\nB. $2+i$')
        render_case(fixture_cli, production_cli, root, 'display-answer-blank', '$$z=$$', class_id=5)
        render_case(fixture_cli, production_cli, root, 'exam-symbols',
            r'集合 $\{x \mid x \leqslant 2\}$，角 $90^{\circ}$，'
            r'向量 $\overrightarrow{OZ}$，直线 $l \perp m$。')
        render_case(fixture_cli, production_cli, root, 'labelled-arrow',
            r'复数 $z=a+bi$ $\xrightarrow{一一对应}$ 点 $Z(a,b)$。')
        render_case(fixture_cli, production_cli, root, 'arrow-with-two-labels',
            r'$A\xrightarrow[under]{\text{旋转 }90^{\circ}}B$')
        render_case(fixture_cli, production_cli, root, 'geometry-answer-blank',
            r'在 $\triangle ABC$ 中，$\angle CAB=60^{\circ}$，$|\overrightarrow{AD}|=$。')
        render_case(fixture_cli, production_cli, root, 'chemistry-thermodynamics',
            r'若 $(\Delta G_2^{\ominus}-\Delta G_1^{\ominus})=RT_2\ln x$，求 $x$。')
        render_case(fixture_cli, production_cli, root, 'supported-symbols',
            r'$\forall x\exists y:\varphi+\Phi+\omega+\rho+\xi+\sigma$；'
            r'$a\cap b\vee c\geqslant d\sim e\uparrow f$；'
            r'$\therefore\max x+\min y+\lg z$')
        for name, raw in [
            ('unclosed-answer', '求 $z='),
            ('arrow-no-label', r'$\xrightarrow$'),
            ('arrow-unclosed-label', r'$\xrightarrow[under]{over$'),
            ('arrow-optional-only', r'$\xrightarrow[under]$'),
            ('arrow-damaged-fraction', r'$\xrightarrow{\frac{a}}$'),
            ('arrow-unknown-command', r'$\xrightarrow{对应}+\foo{x}$'),
            ('annotation-does-not-cover-prose', r'$\xrightarrow{对应}中文$'),
            ('vector-no-argument', r'$\overrightarrow$'),
            ('unknown-formula-command', r'$$\foo{x}$$'),
        ]:
            render_case(fixture_cli, production_cli, root, name, raw,
                class_id=5 if name == 'unknown-formula-command' else 22, valid=False)
        render_case(fixture_cli, production_cli, root, 'truncated-answer-blank', '$z=$',
            valid=False, finish_reason='truncated', stop_reason='token_limit', expected_error='ovis_token_limit')
        render_case(fixture_cli, production_cli, root, 'truncated-chemical-formula',
            r'$$A(g)\rightleftharpoons P(g)$$', class_id=5, valid=False,
            finish_reason='truncated', stop_reason='token_limit', expected_error='ovis_token_limit')


if __name__ == '__main__':
    main()
