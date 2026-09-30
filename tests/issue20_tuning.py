"""公共作业验证实验选项、低分候选、补白回映和配置追溯。"""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from PIL import Image
from layout_integration import ROOT, config, run


def main():
    fixture, production = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix='dococr-tuning-') as temporary:
        root = Path(temporary)
        image = root / 'page.png'
        Image.new('RGB', (100, 200), '#ff0000').save(image)
        setting = config('fixture:layout_tuning')
        setting['execution']['layout_preprocess'] = 'smartresize_area'
        setting['execution']['layout_score_threshold'] = .3
        out = root / 'selected'
        result = run(fixture, setting, image, out)
        assert result.returncode == 0, result.stderr
        doc = json.loads((out / 'document.json').read_text())
        diagnostic = doc['layout_diagnostics']
        assert diagnostic['score_threshold'] == .3
        assert diagnostic['input_transform']['resample'] == 'area'
        assert diagnostic['input_transform']['content_size'] == [400, 800]
        assert diagnostic['input_transform']['pad_offset'] == [200, 0]
        assert diagnostic['candidates'][0]['selected'] is True  # .4 分候选
        assert diagnostic['candidates'][1]['filter_reason'] == 'outside_page'
        assert doc['pages'][0]['blocks'][0]['bbox'] == [0, 0, 100, 200]
        with Image.open(out / doc['pages'][0]['blocks'][0]['content']['resource']) as crop:
            assert crop.size == (100, 200) and crop.getpixel((50, 100)) == (255, 0, 0)
        # RGB张量中的补白为白色，内容为红色；模型收到真实适配结果。
        tensor = (out / diagnostic['raw_tensor_assets']['image']).read_bytes()
        assert struct.unpack_from('<f', tensor, (800*800+400*800+400)*4)[0] == 0
        assert struct.unpack_from('<f', tensor, (800*800+400*800+10)*4)[0] == 1
        manifest = json.loads((out / 'run-manifest.json').read_text())
        assert manifest['effective_parameters']['layout_score_threshold'] == .3
        assert manifest['effective_parameters']['layout_preprocess'] == 'smartresize_area'
        result = subprocess.run([production, '--reexport', str(out / 'document.json'),
                                 '--asset-root', str(out), '--out', str(root / 'rebuilt')],
                                cwd=ROOT, capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        assert json.loads((root / 'rebuilt/document.json').read_text()) == doc
        stripe = Image.new('RGB', (1000, 1))
        stripe.putdata([(255, 255, 255) if x % 2 else (0, 0, 0) for x in range(1000)])
        stripe.resize((1000, 2000), Image.Resampling.NEAREST).save(root / 'stripes.png')
        result = run(fixture, setting, root / 'stripes.png', root / 'downsampled')
        assert result.returncode == 0, result.stderr
        downsampled = json.loads((root / 'downsampled/document.json').read_text())
        raw = (root / 'downsampled' / downsampled['layout_diagnostics']['raw_tensor_assets']['image']).read_bytes()
        # 该输出位置覆盖原图黑/白/黑三列，BOX均值为85；双线性会得到191。
        value = struct.unpack_from('<f', raw, (400*800+400)*4)[0]
        assert abs(value-85/255) < 1e-7, value
        setting['execution']['layout_preprocess'] = 'smartresize_lanczos'
        result = run(fixture, setting, root / 'stripes.png', root / 'antialiased')
        assert result.returncode == 0, result.stderr
        antialiased = json.loads((root / 'antialiased/document.json').read_text())
        assert antialiased['layout_diagnostics']['input_transform']['resample'] == 'lanczos'
        raw = (root / 'antialiased' / antialiased['layout_diagnostics']['raw_tensor_assets']['image']).read_bytes()
        value = struct.unpack_from('<f', raw, (400*800+400)*4)[0]
        assert abs(value-128/255) < 1e-7, value
        setting['execution']['layout_score_threshold'] = .5
        result = run(fixture, setting, image, root / 'filtered')
        assert result.returncode == 0, result.stderr
        filtered = json.loads((root / 'filtered/document.json').read_text())
        assert filtered['pages'][0]['blocks'] == []
        assert filtered['layout_diagnostics']['candidates'][0]['filter_reason'] == 'below_score_threshold'
        for field, value in [('layout_preprocess', 'unknown'), ('layout_score_threshold', 1.1),
                             ('layout_score_threshold', -.1), ('layout_score_threshold', True)]:
            invalid = config('fixture:layout_tuning')
            invalid['execution'][field] = value
            result = run(fixture, invalid, image, root / ('invalid-' + str(value)))
            assert result.returncode == 3 and 'invalid_layout_parameter' in result.stderr


if __name__ == '__main__':
    main()
