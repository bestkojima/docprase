"""公共 CLI：完整多行公式可导出，损坏环境仍回退并保留原文。"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from PIL import Image
from printed_page_integration import ROOT, config


def main():
    fixture_cli, production_cli = sys.argv[1:3]
    with tempfile.TemporaryDirectory(prefix='issue27-formula-') as temporary:
        root = Path(temporary)
        image = root / 'page.png'
        Image.new('RGB', (400, 300), 'white').save(image)
        setting = config('printed_page_structure')
        setting['execution'].update(layout_preprocess='reference', layout_score_threshold=.3)
        cfg = root / 'config.json'
        cfg.write_text(json.dumps(setting))
        cases = [
            (r'$$\begin{aligned}S_n&=\frac{a_1(1-q^n)}{1-q}\\&=2^{54}-1.\end{aligned}$$', True),
            (r'$$\begin{cases}x=1\\y=2\end{cases}$$', True),
            (r'$$\begin{aligned}\begin{matrix}x&y\end{matrix}\end{aligned}$$', True),
            (r'$$\begin{aligned}x\end{matrix}$$', False),
            (r'$$\begin{aligned}x=1$$', False),
            (r'$$\begin{aligned}\frac{a}\\x=1\end{aligned}$$', False),
            (r'$$\begin{unknown}x=1\end{unknown}$$', False),
            (r'$$\begin{aligned}Please solve x\end{aligned}$$', False),
        ]
        for i, (raw, valid) in enumerate(cases):
            trace = root / 'fixture.json'
            trace.write_text(json.dumps(dict(candidates=[[5, .95, 40, 50, 360, 250, 0]],
                outputs=[dict(bbox=[40, 50, 360, 250], text=raw)])))
            job = root / f'job-{i}'
            result = subprocess.run([fixture_cli, '--config', str(cfg), '--input', str(image),
                '--out', str(job)], env=dict(os.environ, DOCOCR_TEST_STRUCTURE_PATH=str(trace)),
                cwd=ROOT, capture_output=True, text=True)
            assert result.returncode == 0, result.stderr
            document = json.loads((job / 'document.json').read_text())
            block = document['pages'][0]['blocks'][0]
            assert block['provenance']['raw_output'] == raw
            assert block['status'] == ('ok' if valid else 'partial'), (raw, block['status'], block['error'])
            markdown = (job / 'document.md').read_text()
            if valid:
                assert block['content']['text'] == raw[2:-2]
                assert '![原图]' not in markdown
            else:
                assert block['error'] == 'invalid_formula_syntax'
                assert '![原图]' in markdown
            exported = root / f'export-{i}'
            result = subprocess.run([production_cli, '--reexport', str(job / 'document.json'),
                '--asset-root', str(job), '--out', str(exported)], cwd=ROOT, capture_output=True, text=True)
            assert result.returncode == 0, result.stderr
            assert (exported / 'document.md').read_bytes() == (job / 'document.md').read_bytes()
            print(f'formula-{i}: PASS', flush=True)


if __name__ == '__main__':
    main()
