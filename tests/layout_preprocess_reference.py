"""固定 HF 参考处理器与 C++ uint8 BICUBIC 张量的数值对照。"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

import numpy as np
from PIL import Image
from transformers.models.pp_doclayout_v3 import image_processing_pp_doclayout_v3 as module
from transformers.models.pp_doclayout_v3.image_processing_pp_doclayout_v3 import PPDocLayoutV3ImageProcessor

ROOT = Path(__file__).resolve().parents[1]
PROCESSOR_HASH = '5b064fa7383dda12b3550448eae77d4f627a102c25e8b4db25d99e85fba4abc6'
assert hashlib.sha256(Path(module.__file__).read_bytes()).hexdigest() == PROCESSOR_HASH
processor = PPDocLayoutV3ImageProcessor(**__import__('json').loads(
    (ROOT / 'tests/fixtures/layout/reference-preprocessor-config.json').read_text()))


def compare(probe, rgb, root, label):
    height, width = rgb.shape[:2]
    raw = root / (label + '.rgb')
    out = root / (label + '.f32')
    raw.write_bytes(rgb.tobytes())
    completed = subprocess.run([str(probe), str(width), str(height), str(raw), str(out)],
                               capture_output=True, text=True)
    assert completed.returncode == 0, (label, completed.returncode, completed.stderr)
    actual = np.fromfile(out, '<f4').reshape(3, 800, 800)
    reference = processor(images=Image.fromarray(rgb, 'RGB'), return_tensors='pt')['pixel_values'].numpy()[0]
    delta = np.abs(actual - reference)
    print(f'{label}: {width}x{height} max_abs={delta.max():.9g} differing={np.count_nonzero(delta)}')
    assert delta.max() <= 1e-7 and not np.count_nonzero(delta), label


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('probe', type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        locked = np.asarray(Image.open(ROOT / 'tests/fixtures/layout/exam-jee-346.jpg').convert('RGB'))
        compare(args.probe, locked, root, 'locked-jpeg-decoded-by-PIL')
        for width, height in [(2, 2), (1, 1), (135, 73), (800, 640), (640, 800), (800, 800)]:
            yy, xx = np.mgrid[:height, :width]
            rgb = np.stack(((xx*17 + yy*29) % 256, (xx*113 + yy*3) % 256,
                            (xx*5 + yy*71) % 256), axis=-1).astype(np.uint8)
            compare(args.probe, rgb, root, f'pattern-{width}x{height}')


if __name__ == '__main__':
    main()
