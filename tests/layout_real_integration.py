"""真实 MNN 公共作业验收；输出逐项结果和失败证据到指定目录。"""
import argparse
import copy
import gzip
import hashlib
import json
from pathlib import Path
import struct
import subprocess

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
REF = ROOT / 'docs/research-evidence/runtime/layout/issue-2'
SOURCE = ROOT / 'tests/fixtures/layout/exam-jee-346.jpg'
LOSSLESS = ROOT / 'tests/fixtures/layout/exam-jee-346-pil-rgb.png'
CONFIG = ROOT / 'configs/layout-plan.example.json'
SCHEMA = json.loads((ROOT / 'schemas/document-ir/document-ir-1.10-image.schema.json').read_text())


def hash_file(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def decode_masks(data):
    magic = b'DOCOCR_MASK_RLE_V1\n'
    assert data.startswith(magic)
    pos = len(magic)
    shape = struct.unpack_from('<III', data, pos)
    pos += 12
    assert shape == (300, 200, 200)
    result = np.empty(shape, dtype='<i4')
    for row in range(300):
        first, count = struct.unpack_from('<II', data, pos)
        pos += 8
        lengths = struct.unpack_from('<' + 'I'*count, data, pos)
        pos += 4*count
        assert first in (0, 1) and sum(lengths) == 40000
        result[row] = np.concatenate([
            np.full(size, (first + i) % 2, dtype='<i4') for i, size in enumerate(lengths)
        ]).reshape(200, 200)
    assert pos == len(data)
    return result


def invoke(binary, config_path, image, out):
    result = subprocess.run([str(binary), '--config', str(config_path), '--input', str(image),
                             '--out', str(out)], cwd=ROOT, capture_output=True, text=True)
    (out.parent / (out.name + '.log')).write_text(
        f'exit_code: {result.returncode}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}')
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('cli', type=Path)
    parser.add_argument('jpeg_probe', type=Path)
    parser.add_argument('out', type=Path)
    parser.add_argument('--direct-probe', type=Path)
    args = parser.parse_args()
    root = args.out.resolve()
    root.mkdir(parents=True, exist_ok=True)
    cfg = json.loads(CONFIG.read_text())
    report = {'source_jpeg_sha256': hash_file(SOURCE), 'lossless_png_sha256': hash_file(LOSSLESS),
              'model_sha256': hash_file(ROOT / 'models/doclayout/PP-DocLayoutV3.mnn')}
    assert report['source_jpeg_sha256'] == 'e8d587b83baade2dbdb3ad3333cfe8bc9a7d9cbf489de4db961058b23343dade'
    assert report['lossless_png_sha256'] == '3f092c959987cc81e83a68d554361d07fce95b08a003cb23ed0ae323e9cd4817'
    assert np.array_equal(np.asarray(Image.open(SOURCE).convert('RGB')),
                          np.asarray(Image.open(LOSSLESS).convert('RGB')))
    assert report['model_sha256'] == cfg['models']['layout']['artifacts'][0]['sha256']

    png_result = invoke(args.cli, CONFIG, LOSSLESS, root / 'png-job')
    assert png_result.returncode == 0, png_result.stderr
    job = root / 'png-job'
    document = json.loads((job / 'document.json').read_text())
    schema_check = subprocess.run(['python3', '-c',
        'import json,jsonschema,sys;jsonschema.validate(json.load(open(sys.argv[1])),json.load(open(sys.argv[2])))',
        str(job / 'document.json'), str(ROOT / 'schemas/document-ir/document-ir-1.10-image.schema.json')],
        capture_output=True, text=True)
    assert schema_check.returncode == 0, schema_check.stderr
    manifest = json.loads((job / 'run-manifest.json').read_text())
    plan = json.loads((job / 'execution-plan.json').read_text())
    diagnostic = document['layout_diagnostics']
    assert plan['config_hash'] == manifest['config_hash']
    assert document['status'] == 'partial' and len(document['pages'][0]['blocks']) == 16
    assert manifest['actual_backend'].startswith('MNN/3.6.1/PP-DocLayoutV3=')
    assert manifest['timing_status']['recognition'] == 'not_run'
    assert all(block['status'] == 'skipped' and block['content']['resource'] for block in document['pages'][0]['blocks'])
    selected_ids = [c['candidate_id'] for c in diagnostic['candidates'] if c['selected']]
    expected_ids = [c['candidate_id'] for c in json.loads((REF / 'candidates.json').read_text()) if c['selected']]
    assert selected_ids == expected_ids
    assets = diagnostic['raw_tensor_assets']
    reference_input = np.frombuffer(gzip.open(REF / 'image.f32.gz', 'rb').read(), '<f4')
    actual_input = np.fromfile(job / assets['image'], '<f4')
    input_max_abs = float(np.max(np.abs(actual_input-reference_input)))
    assert input_max_abs <= 1e-7
    assert np.fromfile(job / assets['im_shape'], '<f4').tolist() == [800., 800.]
    assert np.fromfile(job / assets['scale_factor'], '<f4').tolist() == [1.25, 1.25]
    actual_rows = np.fromfile(job / assets['fetch_name_0'], '<f4').reshape(300,7)
    assert np.fromfile(job / assets['fetch_name_1'], '<i4').tolist() == [300]
    actual_masks = decode_masks((job / assets['fetch_name_2']).read_bytes())
    if args.direct_probe:
        prefix = root / 'direct-one-thread'
        direct = subprocess.run([str(args.direct_probe),
            str(ROOT / 'models/doclayout/PP-DocLayoutV3.mnn'), str(job / assets['image']),
            str(prefix), '640', '640', '1', '1'], capture_output=True, text=True)
        (root / 'direct-one-thread.log').write_text(
            f'exit_code: {direct.returncode}\nstdout:\n{direct.stdout}\nstderr:\n{direct.stderr}')
        assert direct.returncode == 0, direct.stderr
        row_bytes_equal = (job / assets['fetch_name_0']).read_bytes() == (root / 'direct-one-thread.fetch_name_0.bin').read_bytes()
        count_bytes_equal = (job / assets['fetch_name_1']).read_bytes() == (root / 'direct-one-thread.fetch_name_1.bin').read_bytes()
        direct_masks = np.fromfile(root / 'direct-one-thread.fetch_name_2.bin', '<i4').reshape(300,200,200)
        mask_different_pixels = int(np.count_nonzero(actual_masks != direct_masks))
        assert row_bytes_equal and count_bytes_equal and mask_different_pixels == 0
        report['direct_one_thread'] = {'exit_code': 0, 'candidate_bytes_equal': row_bytes_equal,
            'count_bytes_equal': count_bytes_equal, 'mask_different_pixels': mask_different_pixels}
    reference_rows = np.frombuffer(gzip.open(REF / 'normal.candidates.f32.gz', 'rb').read(), '<f4').reshape(300,7)
    reference_masks = np.frombuffer(gzip.open(REF / 'normal.masks.i32.gz', 'rb').read(), '<i4').reshape(300,200,200)
    mask_diff = np.count_nonzero(actual_masks != reference_masks, axis=(1,2))
    assert not np.any(mask_diff[selected_ids])
    assert np.array_equal(actual_rows[selected_ids,0].astype(int), reference_rows[selected_ids,0].astype(int))
    assert np.array_equal(actual_rows[selected_ids,6].astype(int), reference_rows[selected_ids,6].astype(int))
    max_selected_box_diff = float(np.max(np.abs(actual_rows[selected_ids,2:6]-reference_rows[selected_ids,2:6])))
    assert max_selected_box_diff < .001
    page_mask_differences = {}
    for candidate in diagnostic['candidates']:
        if not candidate['selected']:
            continue
        model_mask = np.asarray(Image.open(job / candidate['mask_asset']).convert('L'))
        reference_mask = np.asarray(Image.open(REF / 'page-masks' / f"{candidate['candidate_id']:03d}.png").convert('L'))
        difference = int(np.count_nonzero(model_mask != reference_mask))
        page_mask_differences[str(candidate['candidate_id'])] = difference
        assert difference == 0
    report['png_job'] = {'exit_code': png_result.returncode, 'status': document['status'],
                         'raw_candidate_count': diagnostic['candidate_count'], 'selected_ids': selected_ids,
                         'raw_mask_different_rows_vs_issue2': np.flatnonzero(mask_diff).tolist(),
                         'raw_mask_differing_pixels_vs_issue2': int(mask_diff.sum()),
                         'selected_box_max_abs_vs_issue2': max_selected_box_diff,
                         'selected_page_mask_differences': page_mask_differences,
                         'asset_count': len(list((job / 'assets').iterdir())),
                         'input_tensor_max_abs_vs_issue2': input_max_abs,
                         'runtime': manifest['actual_backend'], 'manifest': 'png-job/run-manifest.json'}

    stb = root / 'stb-decoded.rgb'
    decoder = subprocess.run([str(args.jpeg_probe), str(SOURCE), str(stb)], capture_output=True, text=True)
    assert decoder.returncode == 0
    pil = np.asarray(Image.open(SOURCE).convert('RGB'))
    stb_rgb = np.fromfile(stb, np.uint8).reshape(pil.shape)
    jpeg_delta = np.abs(stb_rgb.astype(np.int16) - pil.astype(np.int16))
    report['jpeg_decode'] = {'max_abs': int(jpeg_delta.max()),
                             'differing_components': int(np.count_nonzero(jpeg_delta)),
                             'differing_pixels': int(np.count_nonzero(np.any(jpeg_delta, axis=2)))}
    jpeg_result = invoke(args.cli, CONFIG, SOURCE, root / 'jpeg-job')
    assert jpeg_result.returncode == 0
    jpeg_document = json.loads((root / 'jpeg-job/document.json').read_text())
    report['jpeg_job'] = {'exit_code': 0, 'selected_count': sum(c['selected'] for c in
                          jpeg_document['layout_diagnostics']['candidates']),
                          'status': jpeg_document['status']}

    failures = {}
    for name, change, expected in [
        ('missing-model', {'root': str(root / 'missing')}, 'artifact_missing'),
        ('wrong-hash', {'sha256': '0'*64}, 'artifact_hash_mismatch'),
    ]:
        bad = copy.deepcopy(cfg)
        bad['models']['layout'].update({k:v for k,v in change.items() if k == 'root'})
        if 'sha256' in change: bad['models']['layout']['artifacts'][0]['sha256'] = change['sha256']
        cfg_path = root / (name + '.json')
        cfg_path.write_text(json.dumps(bad))
        completed = invoke(args.cli, cfg_path, LOSSLESS, root / name)
        assert completed.returncode == 3 and expected in completed.stderr
        failures[name] = {'exit_code': completed.returncode, 'error': expected}
    bogus = root / 'bogus.mnn'
    bogus.write_bytes(b'not a valid MNN model')
    bad = copy.deepcopy(cfg)
    bad['models']['layout']['root'] = str(root)
    bad['models']['layout']['artifacts'][0] = {'path': bogus.name, 'sha256': hash_file(bogus)}
    cfg_path = root / 'invalid-model.json'
    cfg_path.write_text(json.dumps(bad))
    completed = invoke(args.cli, cfg_path, LOSSLESS, root / 'invalid-model')
    assert completed.returncode == 3 and 'layout_artifact_contract_mismatch' in completed.stderr
    failures['invalid-model'] = {'exit_code': completed.returncode, 'error': 'layout_artifact_contract_mismatch'}
    report['failures'] = failures
    (root / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
