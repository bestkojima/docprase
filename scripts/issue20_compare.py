"""固定开发集的 DocLayout 对照实验；原始候选、冻结参照与标注分别保留。

依赖 numpy、scipy、Pillow、opencv-python-headless、shapely；不是官方榜单评分器。
"""
import argparse
import ast
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from copy import deepcopy
import hashlib
import gzip
import inspect
import json
from pathlib import Path
import subprocess
import shutil
from typing import Dict, List, Optional, Tuple, Union
from urllib.request import urlopen

import cv2
import numpy as np
import PIL
from PIL import Image
from scipy.optimize import linear_sum_assignment
from shapely.geometry import box as polygon_box
from shapely.ops import unary_union

from issue17_baseline import DATA, MANIFEST, ROOT, TEXT_CATEGORIES, box, verify_data
from issue20_runtime import freeze_runtime

LABELS = ['abstract', 'algorithm', 'aside_text', 'chart', 'content', 'formula',
          'doc_title', 'figure_title', 'footer', 'footer', 'footnote', 'formula_number',
          'header', 'header', 'image', 'formula', 'number', 'paragraph_title',
          'reference', 'reference_content', 'seal', 'table', 'text', 'text', 'vision_footnote']
LARGE = {3, 5, 6, 15, 17}
REFERENCE_HASHES = {'detection.py': 'de2bb4f14f01619435c65df09272d978619c045c915cba465990dc4c32ac3540',
                    'layout.py': 'b9e0d53738cd9147b2b665a96b4cd38bef8668f75e9c85ba4b8826cd41f02ca2'}
MODEL_HASH = '5f1a43441d70f6843012b47eb294bed7edd3d0ef2344f0074700a38cb2e29c67'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_reference(directory):
    """直接执行冻结源文件中的函数/类 AST；仅移除包导入和计时装饰器。"""
    env = dict(np=np, ndarray=np.ndarray, cv2=cv2, Dict=Dict, List=List, Optional=Optional,
               Tuple=Tuple, Union=Union, Boxes=list, Number=Union[int, float])
    for name in ('detection', 'layout'):
        source_path = directory / (name + '.py')
        if not source_path.exists():
            directory.mkdir(parents=True, exist_ok=True)
            folder = 'object_detection' if name == 'detection' else 'layout_analysis'
            url = 'https://raw.githubusercontent.com/PaddlePaddle/PaddleX/' \
                f'ffb64904d23708863ff5b8da312a5cbd52a7f462/paddlex/inference/models/{folder}/processors.py'
            source_path.write_bytes(urlopen(url, timeout=30).read())
        if sha(directory / (name + '.py')) != REFERENCE_HASHES[name + '.py']:
            raise ValueError('冻结参照文件哈希不符')
        tree = ast.parse((directory / (name + '.py')).read_text())
        nodes = [n for n in tree.body if isinstance(n, ast.FunctionDef) or
                 isinstance(n, ast.ClassDef) and n.name == 'LayoutAnalysisProcess']
        for node in ast.walk(ast.Module(body=nodes, type_ignores=[])):
            if isinstance(node, (ast.FunctionDef, ast.ClassDef)):
                node.decorator_list = []
        exec(compile(ast.fix_missing_locations(ast.Module(body=nodes, type_ignores=[])),
                     str(directory / (name + '.py')), 'exec'), env)
    return env


def kind(class_id):
    return 'formula' if class_id in (5, 15) else 'table' if class_id == 21 else \
        'image' if class_id in (3, 14, 20) else 'text'


def area(b):
    return max(0, b[2]-b[0]) * max(0, b[3]-b[1])


def intersection(a, b):
    return max(0, min(a[2], b[2])-max(a[0], b[0])) * max(0, min(a[3], b[3])-max(a[1], b[1]))


def local_filter(rows, size, threshold=.5, rounded=False, outer=True):
    """独立复现现行规则；结果须与公共 CLI 候选 ID 一致后才用于实验。"""
    w, h = size
    records = []
    for idx, row in enumerate(rows):
        cid, score = int(row[0]), float(row[1])
        original = list(map(float, row[2:6]))
        b = np.round(original).tolist() if rounded else original
        crop = [int(np.floor(max(0, b[0]))), int(np.floor(max(0, b[1]))),
                int(np.ceil(min(w, b[2]))), int(np.ceil(min(h, b[3])))]
        if score < threshold or not area(b) or not area(crop):
            continue
        records.append(dict(id=idx, cls_id=cid, score=score, bbox=crop, raw=b,
                            rank=int(row[6]), type=kind(cid)))
    kept = []
    for c in sorted(records, key=lambda c: (c['score'], c['id']), reverse=True):
        for p in kept:
            a, b = c['raw'], p['raw']
            inter = max(0, min(a[2], b[2])-max(a[0], b[0])+1) * \
                max(0, min(a[3], b[3])-max(a[1], b[1])+1)
            union = (a[2]-a[0]+1)*(a[3]-a[1]+1) + (b[2]-b[0]+1)*(b[3]-b[1]+1)-inter
            if inter / union >= (.6 if c['cls_id'] == p['cls_id'] else .98):
                break
        else:
            kept.append(c)
    if len(kept) > 1:
        reduced = [c for c in kept if c['cls_id'] != 14 or
                   area(c['bbox']) <= (.82 if w > h else .93)*w*h]
        kept = reduced or kept
    contained = {c['id'] for c in kept for p in kept if c != p and p['cls_id'] in LARGE
                 and not (c['cls_id'] == 5 and p['cls_id'] != 5) and
                 intersection(c['raw'], p['raw']) / area(c['raw']) >= .9}
    active = {c['id']: c for c in kept if c['id'] not in contained}
    if outer:
        active = {i: c for i, c in active.items() if c['cls_id'] != 18}
        for i in sorted(active):
            if i not in active:
                continue
            for j in sorted(k for k in active if k > i):
                if i not in active:
                    break
                a, b = active[i], active[j]
                small = min(area(a['bbox']), area(b['bbox']))
                inter = intersection(a['bbox'], b['bbox'])
                if inter / small <= .7:
                    continue
                # 当前 core 的 label 是细类别，归属规划则使用 canonical_label。
                la, lb = LABELS[a['cls_id']], LABELS[b['cls_id']]
                protected = any(parent['cls_id'] == 21 and LABELS[child['cls_id']] in ('text', 'formula')
                                and inter / area(child['bbox']) >= .9
                                for parent, child in ((a, b), (b, a))) or \
                    any(LABELS[f['cls_id']] == 'formula' and LABELS[t['cls_id']] == 'text'
                        and inter / area(f['bbox']) >= .85 for f, t in ((a, b), (b, a)))
                annotations = {'figure_title', 'footnote', 'vision_footnote'}
                protected |= bool({la, lb} & annotations and small / max(area(a['bbox']), area(b['bbox'])) < .8)
                special = {'image', 'table', 'chart', 'seal'}
                protected |= bool({la, lb} & special and la != lb and
                                  ('table' not in (la, lb) or la in special and lb in special))
                if not protected:
                    del active[j if area(a['bbox']) >= area(b['bbox']) else i]
    return list(active.values())


def official_filter(env, rows, size, threshold, shape='rect', masks=None):
    process = env['LayoutAnalysisProcess'](LABELS, [800, 800])
    out = process.apply(rows.copy(), size, float(threshold), True, (1., 1.),
                        {c: 'large' if c in LARGE else 'union' for c in range(25)},
                        masks=masks, layout_shape_mode=shape)
    out = env['filter_boxes'](out, layout_shape_mode=shape)
    results = []
    for c in out:
        ids = np.where((rows[:, 0] == c['cls_id']) & (rows[:, 1] == c['score']))[0]
        assert len(ids) == 1, '不能唯一恢复候选来源'
        result = dict(id=int(ids[0]), cls_id=c['cls_id'], score=c['score'],
                      bbox=c['coordinate'], rank=int(rows[ids[0], 6]), type=kind(c['cls_id']))
        if 'polygon_points' in c:
            result['polygon'] = c['polygon_points'].tolist()
        results.append(result)
    return results


def score(annotation, predictions):
    """类别一对一 IoU>=.5；另报告同类别框并集覆盖与多余框，避免分块粒度混淆。"""
    refs = [dict(id=a['anno_id'], bbox=box(a['poly']),
                 type='text' if a['category_type'] in TEXT_CATEGORIES else
                 {'equation_isolated': 'formula', 'figure': 'image'}.get(a['category_type'], a['category_type']))
            for a in annotation['layout_dets'] if not a.get('ignore') and
            a['category_type'] in TEXT_CATEGORIES | {'equation_isolated', 'table', 'figure'}]
    results = {}
    for category in ('text', 'formula', 'table', 'image'):
        expected = [r for r in refs if r['type'] == category]
        predicted = [p for p in predictions if p['type'] == category]
        weights = np.zeros((len(expected), len(predicted)))
        for i, r in enumerate(expected):
            for j, p in enumerate(predicted):
                inter = intersection(r['bbox'], p['bbox'])
                weights[i, j] = inter / (area(r['bbox'])+area(p['bbox'])-inter)
        # 首先最大化可匹配数，再以 IoU 打破平局。
        matches = []
        if weights.size:
            ii, jj = linear_sum_assignment((weights >= .5)*1000 + weights, maximize=True)
            matches = [(expected[i]['id'], predicted[j]['id']) for i, j in zip(ii, jj) if weights[i, j] >= .5]
        union = unary_union([polygon_box(*p['bbox']) for p in predicted])
        coverage = [union.intersection(polygon_box(*r['bbox'])).area / area(r['bbox']) for r in expected]
        truth = unary_union([polygon_box(*r['bbox']) for r in expected])
        extras = sum(truth.intersection(polygon_box(*p['bbox'])).area / area(p['bbox']) < .5 for p in predicted)
        results[category] = dict(reference=len(expected), predicted=len(predicted), matched=len(matches),
                                 covered80=sum(c >= .8 for c in coverage), coverage_sum=sum(coverage),
                                 low_purity=extras, matches=matches)
    results['total'] = {k: sum(r[k] for r in results.values()) for k in
                        ('reference', 'predicted', 'matched', 'covered80', 'coverage_sum', 'low_purity')}
    return results


def recognition_units(predictions, inline_threshold=.85, table_threshold=.9):
    """依当前公共作业的唯一父块规则折叠识别任务；保留版面候选用于审计。"""
    owners = {}
    for c in predictions:
        parents = [p for p in predictions if c != p and p['type'] == 'table' and
                   c['type'] in ('text', 'formula') and area(p['bbox']) > area(c['bbox']) and
                   intersection(c['bbox'], p['bbox']) / area(c['bbox']) >= table_threshold]
        parents.sort(key=lambda p: area(p['bbox']))
        if parents and (len(parents) == 1 or area(parents[0]['bbox']) < area(parents[1]['bbox'])):
            owners[c['id']] = parents[0]['id']
    for c in predictions:
        if c['type'] != 'formula' or c['id'] in owners:
            continue
        parents = [p for p in predictions if p['type'] == 'text' and p['cls_id'] not in (11, 16)
                   and p['id'] not in owners and area(p['bbox']) > area(c['bbox']) and
                   intersection(c['bbox'], p['bbox']) / area(c['bbox']) >= inline_threshold]
        parents.sort(key=lambda p: area(p['bbox']))
        if parents and (len(parents) == 1 or area(parents[0]['bbox']) < area(parents[1]['bbox'])):
            owners[c['id']] = parents[0]['id']
    return [p for p in predictions if p['id'] not in owners], owners


def make_input(mode, image, output):
    w, h = image.size
    transform = None
    if mode == 'reference' or mode == 'letterbox_bilinear':
        raw = output / 'source.rgb'
        raw.write_bytes(image.tobytes())
        command = [str(ROOT / 'build/dococr_layout_preprocess_probe'), str(w), str(h), str(raw), str(output / 'image.f32')]
        if mode == 'letterbox_bilinear':
            command.append('letterbox')
        subprocess.run(command, check=True)
        raw.unlink()
    elif mode in ('stretch_area', 'letterbox_area', 'letterbox_bicubic', 'letterbox_lanczos'):
        if mode == 'stretch_area':
            canvas = image.resize((800, 800), Image.Resampling.BOX)
        else:
            scale = min(800/w, 800/h)
            cw, ch = round(w*scale), round(h*scale)
            canvas = Image.new('RGB', (800, 800), 'white')
            resample = {'letterbox_area': Image.Resampling.BOX,
                        'letterbox_bicubic': Image.Resampling.BICUBIC,
                        'letterbox_lanczos': Image.Resampling.LANCZOS}[mode]
            canvas.paste(image.resize((cw, ch), resample), ((800-cw)//2, (800-ch)//2))
        (np.asarray(canvas).transpose(2, 0, 1).astype('<f4') / 255).tofile(output / 'image.f32')
    elif mode == 'smart_area':
        # 保留模型原有800x800几何，先面积下采样至长边1600，减少强下采样混叠。
        scale = min(1., 1600/max(w, h))
        resized = image.resize((round(w*scale), round(h*scale)), Image.Resampling.BOX)
        raw = output / 'source.rgb'
        raw.write_bytes(resized.tobytes())
        subprocess.run([str(ROOT / 'build/dococr_layout_preprocess_probe'), str(resized.width),
                        str(resized.height), str(raw), str(output / 'image.f32')], check=True)
        raw.unlink()
    else:
        raise ValueError(mode)
    if mode.startswith('letterbox'):
        scale = min(800/w, 800/h)
        cw, ch = round(w*scale), round(h*scale)
        transform = dict(scale_x=cw/w, scale_y=ch/h, pad_x=(800-cw)//2, pad_y=(800-ch)//2)
    return transform


def infer(mode, spec, out, probe, environment, provenance, refresh_cache=False):
    folder = out / mode / spec['id']
    folder.mkdir(parents=True, exist_ok=True)
    metadata = folder / 'input.json'
    mask_path = folder / 'raw.fetch_name_2.bin'
    complete = metadata.exists() and (folder / 'raw.fetch_name_0.bin').exists() and \
        (folder / 'raw.fetch_name_1.bin').exists() and \
        (mask_path.exists() or mask_path.with_suffix('.bin.gz').exists())
    expected = dict(provenance, mode=mode)
    if complete and json.loads(metadata.read_text()).get('provenance') != expected:
        if not refresh_cache:
            raise ValueError(f'缓存执行来源不一致，使用 --refresh-cache 重新推理：{folder}')
        complete = False
    if not complete:
        image = Image.open(DATA / spec['image_path']).convert('RGB')
        preprocessing_probe = ROOT/'build/dococr_layout_preprocess_probe'
        if sha(preprocessing_probe) != provenance['preprocessing_probe_sha256']:
            raise ValueError('预处理运行前 probe 发生变化')
        transform = make_input(mode, image, folder)
        if sha(preprocessing_probe) != provenance['preprocessing_probe_sha256']:
            raise ValueError('预处理运行期间 probe 发生变化')
        h, w = (800, 800) if transform else (spec['height'], spec['width'])
        command = [str(probe), str(ROOT / 'models/doclayout/PP-DocLayoutV3.mnn'),
                   str(folder / 'image.f32'), str(folder / 'raw'), str(h), str(w), '1', '1']
        if sha(ROOT/'models/doclayout/PP-DocLayoutV3.mnn') != MODEL_HASH:
            raise ValueError('推理前模型制品发生变化')
        result = subprocess.run(command, env=environment, capture_output=True, text=True)
        (folder / 'run.log').write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stderr)
        with mask_path.open('rb') as source, gzip.open(mask_path.with_suffix('.bin.gz'), 'wb', compresslevel=1) as target:
            shutil.copyfileobj(source, target)
        mask_path.unlink()
        if sha(ROOT/'models/doclayout/PP-DocLayoutV3.mnn') != MODEL_HASH:
            raise ValueError('推理期间模型制品发生变化')
        metadata.write_text(json.dumps(dict(mode=mode, source_sha256=spec['image_sha256'],
                                           input_sha256=sha(folder/'image.f32'), transform=transform, command=command,
                                           provenance=expected, outputs={p.name: sha(p) for p in
                                               [folder/'raw.fetch_name_0.bin', folder/'raw.fetch_name_1.bin',
                                                mask_path.with_suffix('.bin.gz')]})))
    info = json.loads(metadata.read_text())
    assert info['source_sha256'] == spec['image_sha256'] and info['input_sha256'] == sha(folder/'image.f32')
    assert all(sha(folder/name) == digest for name, digest in info['outputs'].items()), '缓存输出哈希不符'
    rows = np.fromfile(folder / 'raw.fetch_name_0.bin', '<f4').reshape(300, 7)
    count = int(np.fromfile(folder / 'raw.fetch_name_1.bin', '<i4')[0])
    rows = rows[:count].copy()
    t = info['transform']
    if t:
        rows[:, [2, 4]] = (rows[:, [2, 4]]-t['pad_x']) / t['scale_x']
        rows[:, [3, 5]] = (rows[:, [3, 5]]-t['pad_y']) / t['scale_y']
    print(mode, spec['id'], 'raw', count, flush=True)
    return rows


def compact_report(report, selected):
    result = {k: v for k, v in report.items() if k != 'pages'}
    result['selected'] = selected
    result['pages'] = {}
    baseline_metrics = {}
    result['baseline'] = dict(description='同一现行后处理：18页reference/0.5，odb-11与odb-17为letterbox_bilinear/0.5',
                             metrics=baseline_metrics)
    changes = dict(newly_matched=[], newly_unmatched=[])
    for page_id, variants in report['pages'].items():
        chosen = variants[selected]
        result['pages'][page_id] = dict(
            metrics={k: v['metrics']['total'] for k, v in variants.items()},
            selected={k: v for k, v in chosen.items() if k != 'metrics'})
        baseline = ('letterbox_bilinear' if page_id in ('odb-11', 'odb-17') else 'reference') + ':protected:0.5'
        if baseline not in variants:
            continue
        for category, metric in variants[baseline]['metrics'].items():
            baseline_metrics.setdefault(category, Counter()).update({k: v for k, v in metric.items() if k != 'matches'})
        def matched(variant):
            return {(category, str(annotation_id)) for category, m in variant['metrics'].items()
                    if category != 'total' for annotation_id, _ in m['matches']}
        before, after = matched(variants[baseline]), matched(chosen)
        for key, values in [('newly_matched', after-before), ('newly_unmatched', before-after)]:
            changes[key].extend(dict(page_id=page_id, category=c, annotation_id=a) for c, a in sorted(values))
    result['annotation_changes'] = changes
    for metric in baseline_metrics.values():
        metric['f1'] = 2*metric['matched']/(metric['reference']+metric['predicted'])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=ROOT / 'output/issue-20/experiments')
    parser.add_argument('--reference', type=Path, default=ROOT / '.scratch/issue20-reference')
    parser.add_argument('--only', nargs='+')
    parser.add_argument('--modes', nargs='+', default=['reference', 'letterbox_bilinear', 'letterbox_area',
                                                     'stretch_area', 'smart_area', 'letterbox_bicubic', 'letterbox_lanczos'])
    parser.add_argument('--workers', type=int, default=2)
    parser.add_argument('--polygons', action='store_true', help='追加冻结官方 auto 多边形处理对照')
    parser.add_argument('--summary', type=Path, help='另外保存可提交的紧凑证据')
    parser.add_argument('--selected', default='letterbox_lanczos:protected:0.2')
    parser.add_argument('--refresh-cache', action='store_true', help='显式重跑执行来源缺失或不一致的旧缓存')
    args = parser.parse_args()
    manifest = json.loads(MANIFEST.read_text())
    annotations = json.loads((DATA / 'OmniDocBench.json').read_text())
    verify_data(manifest, annotations)
    if sha(ROOT/'models/doclayout/PP-DocLayoutV3.mnn') != MODEL_HASH:
        raise ValueError('模型制品哈希不符')
    env = load_reference(args.reference)
    probe, environment, runtime = freeze_runtime(ROOT/'build/dococr_layout_mnn_probe', args.out.resolve()/'.runtime')
    provenance = dict(model_sha256=MODEL_HASH, runtime=runtime, pillow=PIL.__version__, numpy=np.__version__,
                      preprocessing_probe_sha256=sha(ROOT/'build/dococr_layout_preprocess_probe'),
                      make_input_sha256=hashlib.sha256(inspect.getsource(make_input).encode()).hexdigest())
    jobs = [(mode, spec, annotation) for mode in args.modes
            for spec, annotation in zip(manifest['pages'], annotations) if not args.only or spec['id'] in args.only]
    report = dict(role='development', dataset_sha256=sha(DATA/'OmniDocBench.json'),
                  model_sha256=sha(ROOT/'models/doclayout/PP-DocLayoutV3.mnn'),
                  reference_hashes=REFERENCE_HASHES, script_sha256=sha(Path(__file__)), pages={}, aggregate={},
                  probe_sha256=sha(ROOT/'build/dococr_layout_mnn_probe'),
                  preprocessing_probe_sha256=sha(ROOT/'build/dococr_layout_preprocess_probe'))
    report['inference_provenance'] = provenance

    def evaluate(job):
        mode, spec, annotation = job
        rows = infer(mode, spec, args.out, probe, environment, provenance, args.refresh_cache)
        variants = {}
        for threshold in (.15, .2, .25, .3, .4, .5, .6, .7):
            for policy in ('protected', 'rounded', 'official_rect'):
                predictions = official_filter(env, rows, (spec['width'], spec['height']), threshold) \
                    if policy == 'official_rect' else local_filter(rows, (spec['width'], spec['height']),
                                                                 threshold, policy == 'rounded')
                key = f'{mode}:{policy}:{threshold}'
                units, owners = recognition_units(predictions)
                variants[key] = dict(metrics=score(annotation, units), predictions=predictions, owners=owners)
        if args.polygons:
            folder = args.out / mode / spec['id']
            path = folder / 'raw.fetch_name_2.bin'
            masks = np.fromfile(path, '<i4') if path.exists() else \
                np.frombuffer(gzip.decompress(path.with_suffix('.bin.gz').read_bytes()), '<i4')
            masks = masks.reshape(300, 200, 200)
            if mode.startswith('letterbox'):
                t = json.loads((folder / 'input.json').read_text())['transform']
                # 将模型画布 mask 重采样至原页对应的200x200坐标系。
                xx = np.clip(((np.arange(200)+.5)*spec['width']/200*t['scale_x']+t['pad_x'])/4, 0, 199).astype(int)
                yy = np.clip(((np.arange(200)+.5)*spec['height']/200*t['scale_y']+t['pad_y'])/4, 0, 199).astype(int)
                masks = masks[:, yy[:, None], xx[None, :]]
            for threshold in (.2, .3, .5):
                predictions = official_filter(env, rows, (spec['width'], spec['height']), threshold,
                                              shape='auto', masks=masks[:len(rows)])
                units, owners = recognition_units(predictions)
                variants[f'{mode}:official_auto:{threshold}'] = dict(metrics=score(annotation, units),
                                                                   predictions=predictions, owners=owners)
        if mode == 'letterbox_lanczos':
            predictions = variants[f'{mode}:protected:0.2']['predictions']
            for inline in (.7, .85, .9, 1.):
                for table in (.85, .9, 1.):
                    units, owners = recognition_units(predictions, inline, table)
                    variants[f'ownership:inline={inline}:table={table}'] = dict(metrics=score(annotation, units),
                                                                               predictions=predictions, owners=owners)
        return spec['id'], variants

    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        for page_id, variants in pool.map(evaluate, jobs):
            report['pages'].setdefault(page_id, {}).update(variants)
    for variants in report['pages'].values():
        for key, value in variants.items():
            for category, metric in value['metrics'].items():
                summary = report['aggregate'].setdefault(key, {}).setdefault(category, Counter())
                summary.update({k: v for k, v in metric.items() if k != 'matches'})
    for summary in report['aggregate'].values():
        for metric in summary.values():
            metric['f1'] = 2*metric['matched'] / (metric['reference']+metric['predicted']) if metric['reference']+metric['predicted'] else 1
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n')
    if args.summary:
        args.summary.parent.mkdir(parents=True, exist_ok=True)
        args.summary.write_text(json.dumps(compact_report(report, args.selected), ensure_ascii=False, indent=2)+'\n')
    for key, summary in sorted(report['aggregate'].items(), key=lambda x: x[1]['total']['f1'], reverse=True):
        m = summary['total']
        print(key, 'F1', round(m['f1'], 4), 'TP', m['matched'], 'pred', m['predicted'],
              'covered80', m['covered80'], 'low_purity', m['low_purity'], flush=True)


if __name__ == '__main__':
    main()
