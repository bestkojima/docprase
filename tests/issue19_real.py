"""真实模型整页回归；需先构建生产 CLI 并下载固定 MNN 制品。"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'tests/fixtures/layout/exam-jee-346-pil-rgb.png'
REFERENCE = ROOT / 'docs/research-evidence/runtime/layout/issue-2/candidates.json'


def main():
    cli = Path(sys.argv[1]).resolve()
    baseline = json.loads(REFERENCE.read_text())
    with tempfile.TemporaryDirectory(prefix='dococr-issue19-real-') as tmp:
        out = Path(tmp) / 'job'
        run = subprocess.run([str(cli), '--config', 'configs/layout-plan.example.json',
                              '--input', str(SOURCE), '--out', str(out)], cwd=ROOT,
                             capture_output=True, text=True)
        assert run.returncode == 0, run.stderr
        doc = json.loads((out / 'document.json').read_text())
        candidates = doc['layout_diagnostics']['candidates']
        expected_ids = [c['candidate_id'] for c in baseline if c['selected']]
        actual_ids = [c['candidate_id'] for c in candidates if c['selected']]
        assert actual_ids == expected_ids == list(range(16))
        assert sorted(b['candidate_id'] for b in doc['pages'][0]['layout_blocks']) == actual_ids
        assert len(doc['pages'][0]['blocks']) == len(doc['pages'][0]['reading_order']) == 16
        assert all(c['filter_reason'] == 'below_score_threshold'
                   for c in candidates if not c['selected'])
        assert (out / 'document.md').is_file()
        rebuilt = Path(tmp) / 'rebuilt'
        run = subprocess.run([str(cli), '--reexport', str(out / 'document.json'),
                              '--asset-root', str(out), '--out', str(rebuilt)], cwd=ROOT,
                             capture_output=True, text=True)
        assert run.returncode == 0, run.stderr
        assert json.loads((rebuilt / 'document.json').read_text()) == doc
        assert (rebuilt / 'document.md').read_bytes() == (out / 'document.md').read_bytes()
        print('真实整页通过：16 个块、16 个阅读顺序项，候选与基线一致，重新导出一致')


if __name__ == '__main__':
    main()
