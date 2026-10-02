"""公共 CLI 受控反例/真实整页与冻结官方矩形、auto 处理链对照。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import struct
import subprocess
import sys

import cv2
import numpy as np
from PIL import Image
import PIL
import shapely

from issue28_reference import ROOT, SOURCES, load_reference, run_reference, save, sha


def read_masks(path):
    with path.open('rb') as stream:
        if stream.readline() != b'DOCOCR_MASK_RLE_V1\n':
            raise ValueError('mask RLE magic 不符')
        def integer():
            return struct.unpack('<I', stream.read(4))[0]
        if [integer() for _ in range(3)] != [300, 200, 200]:
            raise ValueError('mask RLE shape 不符')
        masks = np.zeros((300, 40000), dtype=np.int32)
        for row in masks:
            first, runs = integer(), integer()
            if first > 1 or not 0 < runs <= 40000:
                raise ValueError('mask RLE runs 不符')
            offset = 0
            for i in range(runs):
                count = integer()
                if not count or offset + count > 40000:
                    raise ValueError('mask RLE length 不符')
                row[offset:offset+count] = (first+i) % 2
                offset += count
            if offset != 40000:
                raise ValueError('mask RLE size 不符')
        if stream.read(1):
            raise ValueError('mask RLE 尾部数据')
    return masks.reshape(300, 200, 200)


def write_masks(path, rows, size):
    with path.open('wb') as stream:
        stream.write(b'DOCOCR_MASK_RLE_V1\n' + struct.pack('<III', 300, 200, 200))
        for index in range(300):
            mask = np.zeros((200, 200), dtype=np.int32)
            if index < len(rows):
                x0, y0, x1, y1 = rows[index][2:6]
                x0, x1 = [int(np.clip(round(x*200/size[0]), 0, 200)) for x in (x0, x1)]
                y0, y1 = [int(np.clip(round(y*200/size[1]), 0, 200)) for y in (y0, y1)]
                mask[y0:y1, x0:x1] = 1
            flat = mask.ravel()
            edges = np.r_[0, np.flatnonzero(flat[1:] != flat[:-1])+1, len(flat)]
            counts = np.diff(edges)
            stream.write(struct.pack('<II', int(flat[0]), len(counts)))
            stream.write(counts.astype('<u4').tobytes())


def invoke(command, folder, env=None):
    result = subprocess.run(list(map(str, command)), cwd=ROOT, env=env, capture_output=True, text=True)
    save(folder / 'command.json', dict(command=list(map(str, command)), returncode=result.returncode))
    (folder / 'stdout.log').write_text(result.stdout)
    (folder / 'stderr.log').write_text(result.stderr)
    if result.returncode:
        raise RuntimeError(f'公共作业失败：{folder}: {result.stderr}')


def compare_job(job, env, labels):
    document = json.loads((job / 'document.json').read_text())
    diag = document['layout_diagnostics']
    assets = diag['raw_tensor_assets']
    count = int(np.fromfile(job / assets['fetch_name_1'], '<i4')[0])
    rows = np.fromfile(job / assets['fetch_name_0'], '<f4').reshape(300, 7)[:count].copy()
    masks = read_masks(job / assets['fetch_name_2'])[:count]
    page = document['pages'][0]
    size = tuple(page['raster_size'])
    transform = diag.get('input_transform')
    if transform:
        a, _, tx, _, b, ty = transform['canvas_to_page_affine']
        rows[:, [2, 4]] = rows[:, [2, 4]].astype(np.float64)*a+tx
        rows[:, [3, 5]] = rows[:, [3, 5]].astype(np.float64)*b+ty
        # 将画布 mask 投到原页的 200x200 网格；只用于参照几何，不写回生产裁图。
        xx = np.clip(np.floor(((np.arange(200)+.5)*size[0]/200-tx)/a/4).astype(int), 0, 199)
        yy = np.clip(np.floor(((np.arange(200)+.5)*size[1]/200-ty)/b/4).astype(int), 0, 199)
        masks = masks[:, yy[:, None], xx[None, :]]
    # 用公共产物核对浮点框、rank、原 mask 行；不以旧 Python local_filter 代替生产。
    for i, candidate in enumerate(diag['candidates']):
        if candidate['candidate_id'] != i or candidate['mask_row'] != i or candidate['rank'] != rows[i, 6]:
            raise ValueError('公共产物候选来源错位')
        if not np.allclose(rows[i, 2:6], candidate['original_bbox'], rtol=0, atol=.002):
            raise ValueError('公共产物原框回映不一致')
    result = dict(document_sha256=sha(job/'document.json'), run_manifest_sha256=sha(job/'run-manifest.json'),
                  raw_assets={k: dict(path=v, sha256=sha(job/v)) for k, v in assets.items()},
                  size=size, threshold=diag['score_threshold'], input_transform=transform,
                  mask_adapter='page_grid_nearest_200' if transform else 'identity',
                  local_candidates=diag['candidates'], local_layout_blocks=page['layout_blocks'],
                  local_regions=page['regions'], local_reading_order=page['reading_order'])
    if any(row[0] >= 25 for row in rows):
        result['official_status'] = 'not_applicable_unknown_class_preserved_locally'
        return result
    for mode in ('rect', 'auto'):
        result[mode] = run_reference(env, labels, rows, masks, size, diag['score_threshold'], mode)
        local = {c['candidate_id'] for c in diag['candidates'] if c['selected']}
        official = {b['candidate_id'] for b in result[mode]['pipeline_output']}
        result[mode]['selection_difference'] = dict(local_only=sorted(local-official), official_only=sorted(official-local))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli', type=Path, required=True)
    parser.add_argument('--fixture-cli', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--reference', type=Path, default=ROOT / '.scratch/issue28-reference')
    parser.add_argument('--real', action='store_true', help='另跑 #19 原整页及冻结开发集 20 页')
    args = parser.parse_args()
    args.out = args.out.resolve()
    # 证据目录不允许复用，避免静默混合运行来源。
    args.out.mkdir(parents=True, exist_ok=False)
    env, labels = load_reference(args.reference)
    report = dict(sources=json.loads(SOURCES.read_text()), labels=labels, production_changed=False,
                  script_sha256={p.name: sha(p) for p in (Path(__file__), ROOT/'scripts/issue28_reference.py')},
                  runtime=dict(python=sys.version, executable=sys.executable, platform=platform.platform(),
                               numpy=np.__version__, opencv=cv2.__version__, pillow=PIL.__version__, shapely=shapely.__version__),
                  binaries={str(p): sha(p) for p in (args.cli, args.fixture_cli,
                            args.cli.parent/'libdococr_c.so', args.fixture_cli.parent/'libdococr_c_test.so')},
                  git_head=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                  working_tree_diff_sha256=hashlib.sha256(subprocess.check_output(['git', 'diff'], cwd=ROOT)).hexdigest(),
                  cases={}, real={})
    config = json.loads((ROOT/'configs/layout-plan.example.json').read_text())
    config['execution'].update(layout_preprocess='reference', layout_score_threshold=.3)
    fixture_config = json.loads((ROOT/'configs/printed-page.example.json').read_text())
    fixture_config['execution'].update(layout_preprocess='reference', layout_score_threshold=.3)
    fixture_config.update(mode='development', backend='fixture:printed_page_structure')
    fixture_config['models'] = json.loads((ROOT/'configs/fixture-plan.example.json').read_text())['models']
    cases = json.loads((ROOT/'tests/fixtures/layout/issue28-cases.json').read_text())
    cases['all_labels'] = dict(rows=[[i, .95-i*.001, 5+(i%5)*38, 5+(i//5)*38,
                                          35+(i%5)*38, 25+(i//5)*38, i] for i in range(25)],
                               local_selected=[i for i in range(25) if i != 18],
                               official_selected=[i for i in range(25) if i != 18])
    for name, case in cases.items():
        folder = args.out / name
        folder.mkdir()
        image = folder/'page.png'
        Image.new('RGB', (200, 200), 'white').save(image)
        save(folder/'config.json', fixture_config)
        write_masks(folder/'masks.rle', case['rows'], (200, 200))
        # 合成转写只为走完公共作业，核验内容归属/资源；不作为模型识别质量证据。
        crops = []
        for row in case['rows']:
            crops.append([max(0, int(np.floor(row[2]))), max(0, int(np.floor(row[3]))),
                          min(200, int(np.ceil(row[4]))), min(200, int(np.ceil(row[5])))])
        outputs = {}
        for i, crop in enumerate(crops):
            text = '<table><tr><td>示例</td></tr></table>' if case['rows'][i][0] == 21 else 'x'
            outputs[tuple(crop)] = text
            for other in crops:
                union = (min(crop[0], other[0]), min(crop[1], other[1]),
                         max(crop[2], other[2]), max(crop[3], other[3]))
                outputs.setdefault(union, text)
        save(folder/'fixture.json', dict(candidates=case['rows'],
             outputs=[dict(bbox=list(b), text=t) for b, t in outputs.items()], mask_rle_path=str(folder/'masks.rle')))
        invoke([args.fixture_cli.resolve(), '--config', folder/'config.json', '--input', image, '--out', folder/'job'],
               folder, dict(os.environ, DOCOCR_TEST_STRUCTURE_PATH=str(folder/'fixture.json')))
        comparison = compare_job(folder/'job', env, labels)
        selected = [c['candidate_id'] for c in comparison['local_candidates'] if c['selected']]
        if selected != case['local_selected']:
            raise ValueError(f'{name}: 本地候选不符合反例规格 {selected}')
        if case['official_selected'] is not None:
            for mode in ('rect', 'auto'):
                selected = sorted(b['candidate_id'] for b in comparison[mode]['pipeline_output'])
                if selected != case['official_selected']:
                    raise ValueError(f'{name}/{mode}: 官方候选不符合反例规格 {selected}')
        save(folder/'comparison.json', comparison)
        report['cases'][name] = dict(path=str(folder/'comparison.json'), sha256=sha(folder/'comparison.json'),
                                    input=case, status='passed')
        print(name, 'passed', flush=True)
    if args.real:
        manifest_path = ROOT/'docs/omnidocbench-20/manifest.json'
        manifest = json.loads(manifest_path.read_text())
        report['dataset_manifest_sha256'] = sha(manifest_path)
        pages = [dict(id='issue19-reference', path=ROOT/'tests/fixtures/layout/exam-jee-346-pil-rgb.png')]
        for page in manifest['pages']:
            path = ROOT/'output/omnidocbench/selected-20'/page['image_path']
            if sha(path) != page['image_sha256']:
                raise ValueError('冻结真实页面哈希不符')
            pages.append(dict(id=page['id'], path=path))
        for page in pages:
            folder = args.out / page['id']
            folder.mkdir()
            setting = json.loads(json.dumps(config))
            setting['execution'].update(layout_preprocess='reference' if page['id']=='issue19-reference' else 'smartresize_lanczos',
                                        layout_score_threshold=.5 if page['id']=='issue19-reference' else .3)
            save(folder/'config.json', setting)
            invoke([args.cli.resolve(), '--config', folder/'config.json', '--input', page['path'], '--out', folder/'job'], folder)
            comparison = compare_job(folder/'job', env, labels)
            save(folder/'comparison.json', comparison)
            report['real'][page['id']] = dict(path=str(folder/'comparison.json'), sha256=sha(folder/'comparison.json'),
                                             image_path=str(page['path']), image_sha256=sha(page['path']),
                                             config_sha256=sha(folder/'config.json'))
            print(page['id'], 'passed', flush=True)
    save(args.out/'report.json', report)


if __name__ == '__main__':
    main()
