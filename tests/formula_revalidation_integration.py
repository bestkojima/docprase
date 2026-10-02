"""公共 CLI：保存输出重新校验，无需模型，保留旧判定和有效来源。"""
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

import jsonschema
from PIL import Image
from printed_page_integration import ROOT, config


def command(*args, code=0):
    result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True)
    assert result.returncode == code, (result.returncode, result.stdout, result.stderr)
    return result


def check_case(root, fixture, production, name, raw, *, class_id=22, historical=False,
               finish_reason='complete', stop_reason='normal', expected_status='ok'):
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
    result = subprocess.run([fixture, '--config', str(cfg), '--input', str(image), '--out', str(job)],
        env=dict(os.environ, DOCOCR_TEST_STRUCTURE_PATH=str(trace)), cwd=ROOT, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    fresh = json.loads((job / 'document.json').read_text())
    source = copy.deepcopy(fresh)
    block = source['pages'][0]['blocks'][0]
    if historical:
        # 构造旧版误判记录，保留实际生成/视觉/停止证据，与旧第 9 题判定一致。
        reason = 'invalid_formula_syntax' if class_id == 5 else 'invalid_inline_formula_syntax'
        source['status'] = source['pages'][0]['status'] = block['status'] = 'partial'
        block['error'] = reason
        block['provenance']['assessment'].update(state='unverified', reason=reason)
        if class_id == 5:
            block['content'].update(format='markdown', text=raw)
        if source['schema_version'] == '1.11':
            source['schema_version'] = '1.10'
    saved = folder / 'saved-document.json'
    saved.write_text(json.dumps(source, ensure_ascii=False))
    old_bytes = saved.read_bytes()
    plain = folder / 'plain-export'
    command(production, '--reexport', str(saved), '--asset-root', str(job), '--out', str(plain))
    assert (plain / 'document.json').read_bytes() == old_bytes
    if historical:
        assert '[待核验：' in (plain / 'document.md').read_text()
    out = folder / 'revalidated'
    command(production, '--revalidate', str(saved), '--asset-root', str(job), '--out', str(out))
    assert saved.read_bytes() == old_bytes
    assert (out / 'previous-document.json').read_bytes() == old_bytes
    document = json.loads((out / 'document.json').read_text())
    version = document['schema_version']
    jsonschema.validate(document, json.loads((ROOT / f'schemas/document-ir/document-ir-{version}-image.schema.json').read_text()))
    new = document['pages'][0]['blocks'][0]
    assert new['status'] == expected_status, (name, new['status'], new['error'])
    assert new['provenance']['raw_output'] == raw
    assert new['provenance']['recognition'] == block['provenance']['recognition']
    assert new['provenance']['visual'] == block['provenance']['visual']
    report = json.loads((out / 'formula-revalidation.json').read_text())
    assert report['execution'] == 'saved_output_no_ocr'
    assert report['source_document_sha256'] == hashlib.sha256(old_bytes).hexdigest()
    assert report['result_document_sha256'] == hashlib.sha256((out / 'document.json').read_bytes()).hexdigest()
    if historical:
        assert document == fresh, (name, document, fresh)
        assert (out / 'document.md').read_bytes() == (job / 'document.md').read_bytes()
        assert len(report['blocks']) == 1 and report['changed_blocks'] == 1
        record = report['blocks'][0]
        assert record['before']['assessment'] == block['provenance']['assessment']
        assert record['after']['assessment'] == new['provenance']['assessment']
        assert record['before']['status'] == 'partial' and record['after']['status'] == 'ok'
    else:
        assert (out / 'document.json').read_bytes() == old_bytes
        assert report['changed_blocks'] == 0
    for item in source['resources']:
        assert (out / item['path']).read_bytes() == (job / item['path']).read_bytes()
    again = folder / 'again'
    command(production, '--reexport', str(out / 'document.json'), '--asset-root', str(out), '--out', str(again))
    assert (again / 'document.json').read_bytes() == (out / 'document.json').read_bytes()
    assert (again / 'document.md').read_bytes() == (out / 'document.md').read_bytes()
    print(f'{name}: PASS', flush=True)


def main():
    fixture, production = sys.argv[1:3]
    with tempfile.TemporaryDirectory(prefix='dococr-formula-revalidation-') as temporary:
        root = Path(temporary)
        check_case(root, fixture, production, 'historical-inline-interval',
            r'区间 $[-1,+\infty)$，以及 $\left(a,b\right]$。', historical=True)
        check_case(root, fixture, production, 'historical-common-symbols',
            r'计算 $6\div 2=3$，集合 $A\cup B$，误差 $\epsilon=0.01$，电阻 $R=10\Omega$。',
            historical=True)
        check_case(root, fixture, production, 'historical-symbol-formula',
            r'$$\prod_{i=1}^{n}x_i+\sum_{i=1}^{n}x_i$$', class_id=5, historical=True)
        check_case(root, fixture, production, 'currency-and-math',
            r'价格 $5，公式 $x^2$。')
        for name, raw in [
            ('unknown-symbol', r'$\invented$'),
            ('definition', r'$\def\x{1}\x$'),
            ('external-link', r'$\href{https://example.invalid}{x}$'),
            ('macro', r'$\newcommand{\x}{1}$'),
            ('symbol-with-broken-group', r'$\epsilon+\frac{1}{$'),
            ('currency-with-broken-formula', r'价格 $5，公式 $\frac{a}$。'),
            ('number-with-unknown-command', r'$2\unknown{x}$'),
            ('number-with-unclosed-command', r'$2 \epsilon'),
            ('number-with-unclosed-escaped-brace', r'$2 \{'),
            ('number-with-unclosed-spacing', r'$2 \,'),
        ]:
            check_case(root, fixture, production, name, raw, expected_status='partial')
        check_case(root, fixture, production, 'historical-display-interval',
            r'$$\left[-1,+\infty\right)$$', class_id=5, historical=True)
        check_case(root, fixture, production, 'historical-formula-sequence',
            r'公式：$x^2+y^2=1$；$z=x+y$。', class_id=5, historical=True)
        check_case(root, fixture, production, 'sequence-broken-second-formula',
            r'公式：$x^2$；$\frac{a}$。', class_id=5, expected_status='partial')
        check_case(root, fixture, production, 'sequence-unclosed-second-formula',
            r'公式：$x^2$；$y', class_id=5, expected_status='partial')
        check_case(root, fixture, production, 'sequence-unknown-command',
            r'公式：$x^2$；$\foo{x}$。', class_id=5, expected_status='partial')
        check_case(root, fixture, production, 'formula-without-math',
            'Please solve this question.', class_id=5, expected_status='partial')
        check_case(root, fixture, production, 'sequence-truncated',
            r'公式：$x^2$；$y=1$。', class_id=5, finish_reason='truncated',
            stop_reason='token_limit', expected_status='partial')
        check_case(root, fixture, production, 'script-unknown-command',
            r'$p_{\foo{x}}$', expected_status='partial')
        check_case(root, fixture, production, 'script-scope-does-not-include-prose',
            r'$p_{sbl}中文$', expected_status='partial')
        check_case(root, fixture, production, 'broken-group-retained', r'$\frac{a}{b$', expected_status='partial')
        check_case(root, fixture, production, 'broken-environment-retained',
            r'$$\begin{matrix}a&b$$', class_id=5, expected_status='partial')
        check_case(root, fixture, production, 'missing-right-retained', r'$\left[a,b$', expected_status='partial')
        check_case(root, fixture, production, 'truncation-retained', r'$[-1,+\infty)$',
            finish_reason='truncated', stop_reason='token_limit', expected_status='partial')
        check_case(root, fixture, production, 'already-valid', r'区间 $(a,b]$。')


if __name__ == '__main__':
    main()
