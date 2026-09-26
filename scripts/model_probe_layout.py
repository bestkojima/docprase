#!/usr/bin/env python3
"""PP-DocLayoutV3-MNN 的单页可复核诊断入口。只生成证据，不充当文档解析器。"""
import argparse
import hashlib
import json
import math
import platform
import subprocess
import sys
from pathlib import Path

import cv2
import numpy as np
from PIL import Image
import torch

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / 'tests/fixtures/layout'
EXPECTED_MODEL_SHA256 = '5f1a43441d70f6843012b47eb294bed7edd3d0ef2344f0074700a38cb2e29c67'
EXAM_SAMPLE_SHA256 = 'e8d587b83baade2dbdb3ad3333cfe8bc9a7d9cbf489de4db961058b23343dade'
REFERENCE_TRANSFORMERS_COMMIT = '27166ea03f12c940f23176a904ab1d2ff1a3dcbb'
REFERENCE_MODEL_REVISION = '97d101e6db2642e162a1d05392d1b0231c91033e'
REFERENCE_MODEL_CONFIG_SHA256 = '3cf834b91d23a756b1519bce4db42c09e852f3e35c35092dd5a3e253a50c071a'
REFERENCE_PREPROCESS_CONFIG_SHA256 = '519fe0187a43a1ca429e3ad8317bab8700f0d5e8fb3a6e3a0a413ffac078ba42'


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2, allow_nan=False) + '\n')


def compare_arrays(a, b):
    difference = np.abs(a.astype(np.float32) - b.astype(np.float32))
    return {'max_abs': float(difference.max()), 'mean_abs': float(difference.mean()),
            'p99_abs': float(np.percentile(difference, 99)), 'different_fraction': float(np.mean(difference != 0))}


def preprocess(image_path, out, max_ref_abs):
    source = Image.open(image_path).convert('RGB')
    rgb = np.asarray(source)
    h, w = rgb.shape[:2]
    # PaddlePaddle / HF 参考规定 RGB、800x800、BICUBIC、1/255、mean=0、std=1。
    # OpenCV INTER_CUBIC 是 MNN 的候选实现；torch bicubic antialias=False 是独立数值参照。
    cv_resized = cv2.resize(rgb, (800, 800), interpolation=cv2.INTER_CUBIC)
    torch_resized = torch.nn.functional.interpolate(
        torch.from_numpy(rgb.copy()).permute(2, 0, 1).unsqueeze(0).float(),
        size=(800, 800), mode='bicubic', align_corners=False,
    ).clamp(0, 255).round().to(torch.uint8).squeeze(0).permute(1, 2, 0).numpy()
    pillow_resized = np.asarray(source.resize((800, 800), Image.Resampling.BICUBIC))
    cv_tensor = np.ascontiguousarray(cv_resized.transpose(2, 0, 1).astype(np.float32) / 255.0)
    torch_tensor = np.ascontiguousarray(torch_resized.transpose(2, 0, 1).astype(np.float32) / 255.0)
    pillow_tensor = np.ascontiguousarray(pillow_resized.transpose(2, 0, 1).astype(np.float32) / 255.0)
    input_path = out / 'image.f32'
    cv_tensor.tofile(input_path)
    comparison = {'reference': 'torch.nn.functional.interpolate bicubic align_corners=False, antialias=False',
                  'candidate': 'OpenCV INTER_CUBIC RGB uint8', 'pillow': 'Pillow BICUBIC RGB uint8',
                  'tolerance_max_abs': max_ref_abs,
                  'candidate_vs_reference': compare_arrays(cv_tensor, torch_tensor),
                  'pillow_vs_reference': compare_arrays(pillow_tensor, torch_tensor),
                  'reference_pass': compare_arrays(cv_tensor, torch_tensor)['max_abs'] <= max_ref_abs}
    write_json(out / 'preprocess-comparison.json', comparison)
    return (h, w), input_path, comparison


def map_mask_to_page(mask, box, page_hw):
    """按固定 HF 处理器的 1/4 缩放、裁剪、最近邻放大规则回映二值 mask。"""
    height, width = page_hw
    x0, y0, x1, y1 = [int(value) for value in box]
    result = np.zeros((height, width), dtype=np.uint8)
    if x1 <= x0 or y1 <= y0:
        return result
    sx, sy = 200 / width, 200 / height
    mx0, mx1 = np.clip([round(x0 * sx), round(x1 * sx)], 0, 200).astype(int)
    my0, my1 = np.clip([round(y0 * sy), round(y1 * sy)], 0, 200).astype(int)
    cropped = mask[my0:my1, mx0:mx1]
    if cropped.size == 0 or not np.any(cropped):
        return result
    expanded = cv2.resize(cropped.astype(np.uint8), (x1 - x0, y1 - y0), interpolation=cv2.INTER_NEAREST)
    px0, px1 = max(0, x0), min(width, x1)
    py0, py1 = max(0, y0), min(height, y1)
    if px1 > px0 and py1 > py0:
        result[py0:py1, px0:px1] = expanded[py0-y0:py1-y0, px0-x0:px1-x0]
    return result


def reference_mask_patch(mask, box, page_hw):
    """固定 HF 处理器 _extract_polygon_points_by_masks 的 crop + resize 数值路径。"""
    height, width = page_hw
    x_min, y_min, x_max, y_max = np.asarray(box).astype(np.int32)
    box_w, box_h = x_max - x_min, y_max - y_min
    if box_w <= 0 or box_h <= 0:
        return np.empty((0, 0), dtype=np.uint8)
    scale_width, scale_height = (800 / width) / 4, (800 / height) / 4
    x_coordinates = [int(round((x_min * scale_width).item())), int(round((x_max * scale_width).item()))]
    y_coordinates = [int(round((y_min * scale_height).item())), int(round((y_max * scale_height).item()))]
    x_start, x_end = np.clip(x_coordinates, 0, mask.shape[1])
    y_start, y_end = np.clip(y_coordinates, 0, mask.shape[0])
    cropped = mask[y_start:y_end, x_start:x_end]
    if cropped.size == 0 or np.sum(cropped) == 0:
        return np.zeros((box_h, box_w), dtype=np.uint8)
    return cv2.resize(cropped.astype(np.uint8), (box_w, box_h), interpolation=cv2.INTER_NEAREST)


def reference_page_mask(mask, box, page_hw):
    """将参考局部 patch 放入页面，仅用于数值对照。"""
    height, width = page_hw
    x0, y0, x1, y1 = np.asarray(box).astype(np.int32)
    result = np.zeros((height, width), dtype=np.uint8)
    patch = reference_mask_patch(mask, box, page_hw)
    px0, px1 = max(0, x0), min(width, x1)
    py0, py1 = max(0, y0), min(height, y1)
    if patch.size and px1 > px0 and py1 > py0:
        result[py0:py1, px0:px1] = patch[py0-y0:py1-y0, px0-x0:px1-x0]
    return result


def evaluate_candidates(boxes, masks, threshold, page_hw, labels=None):
    if boxes.ndim != 2 or boxes.shape[1] != 7 or masks.shape != (len(boxes), 200, 200):
        raise ValueError('output_shape_mismatch')
    if not np.all(np.isfinite(boxes)):
        raise ValueError('nonfinite_candidate')
    if not np.all((masks == 0) | (masks == 1)):
        raise ValueError('mask_value_not_binary')
    labels = labels or {}
    records = []
    height, width = page_hw
    for row, values in enumerate(boxes):
        class_id, score, x0, y0, x1, y1, rank = [float(x) for x in values]
        reasons = []
        if class_id != int(class_id) or int(class_id) not in range(25):
            reasons.append('unknown_class')
        if score < threshold:
            reasons.append('below_score_threshold')
        if x1 <= x0 or y1 <= y0:
            reasons.append('degenerate_box')
        if x0 < 0 or y0 < 0 or x1 > width or y1 > height:
            reasons.append('outside_page')
        if rank != int(rank):
            reasons.append('nonintegral_rank')
        records.append({'candidate_id': row, 'mask_row': row, 'class_id': int(class_id),
                        'class_name': labels.get(str(int(class_id))), 'score': score,
                        'box_xyxy': [x0, y0, x1, y1], 'rank': rank,
                        'mask_nonzero': int(np.count_nonzero(masks[row])),
                        'selected': not reasons, 'reasons': reasons})
    return records


def iou(box, annotation):
    x, y, w, h = annotation
    overlap = max(0, min(box[2], x+w)-max(box[0], x)) * max(0, min(box[3], y+h)-max(box[1], y))
    area = max(0, box[2]-box[0]) * max(0, box[3]-box[1])
    return overlap / (area + w*h - overlap) if area+w*h-overlap else 0.0


def covered_fraction(boxes, annotation, page_hw):
    """细粒度预测框的并集对人工父框的覆盖；不充当同类检测准确率。"""
    height, width = page_hw
    occupancy = np.zeros((height, width), dtype=np.bool_)
    for box in boxes:
        x0, y0, x1, y1 = [int(v) for v in box]
        occupancy[max(0,y0):min(height,y1), max(0,x0):min(width,x1)] = True
    x, y, w, h = annotation
    return float(np.mean(occupancy[y:y+h, x:x+w])) if w > 0 and h > 0 else 0.0


def run_logged(command, log_path, cwd=None):
    try:
        result = subprocess.run(command, capture_output=True, text=True, cwd=cwd, timeout=120)
    except subprocess.TimeoutExpired as error:
        result = subprocess.CompletedProcess(command, 124, error.stdout.decode(errors='replace') if error.stdout else '',
                                             'timeout_after_120_seconds\n' +
                                             (error.stderr.decode(errors='replace') if error.stderr else ''))
    log_path.write_text('command: ' + ' '.join(map(str, command)) + '\nexit_code: ' + str(result.returncode)
                        + '\nstdout:\n' + result.stdout + '\nstderr:\n' + result.stderr)
    return result


def run(args):
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    report = {'overall': 'failed', 'command': sys.argv, 'exit_code': None,
              'checks': {'static': 'not_run', 'runtime': 'not_run', 'reference_preprocess': 'not_run',
                         'reference_model_output': 'not_verified', 'business_quality': 'not_verified'},
              'environment': {'platform': platform.platform(), 'python': sys.version.split()[0],
                              'opencv': cv2.__version__, 'pillow': Image.__version__, 'torch': torch.__version__,
                              'mnn_source_commit': None, 'mnn_version': None},
              'reference': {'transformers_commit': REFERENCE_TRANSFORMERS_COMMIT,
                            'model_config_revision': REFERENCE_MODEL_REVISION},
              'parameters': {'threshold': args.threshold, 'max_ref_abs': args.max_ref_abs, 'backend': 'CPU',
                             'threads': 4, 'target_hw': [800, 800]},
              'tensor_contract': {'inputs': {'image': {'dtype': 'float32', 'shape': [1,3,800,800]},
                                             'im_shape': {'dtype': 'float32', 'shape': [1,2]},
                                             'scale_factor': {'dtype': 'float32', 'shape': [1,2]}},
                                  'outputs': {'fetch_name_0': {'dtype': 'float32', 'shape': [300,7]},
                                              'fetch_name_1': {'dtype': 'int32', 'shape': [1]},
                                              'fetch_name_2': {'dtype': 'int32', 'shape': [300,200,200]}}},
              'errors': []}
    def save():
        report['exit_code'] = 0 if report['overall'] == 'diagnostic_passed' else 1
        write_json(out / 'report.json', report)
    try:
        if not args.model.is_file():
            raise ValueError('model_missing')
        if not args.image.is_file():
            raise ValueError('image_missing')
        mnn = args.mnn.resolve()
        lib = mnn / 'build/libMNN.so'
        info = mnn / 'build/GetMNNInfo'
        if not lib.is_file() or not info.is_file():
            raise ValueError('compiled_mnn_missing')
        report['model'] = {'path': str(args.model.resolve()), 'sha256': sha256(args.model)}
        report['sample'] = {'path': str(args.image.resolve()), 'sha256': sha256(args.image)}
        if args.reference:
            reference_path = args.reference.resolve()
            reference = json.loads(reference_path.read_text())
            if reference.get('image_sha256') != report['sample']['sha256']:
                raise ValueError('reference_sample_hash_mismatch')
        elif report['sample']['sha256'] == EXAM_SAMPLE_SHA256:
            reference_path = FIXTURES / 'exam-jee-346-reference.json'
            reference = json.loads(reference_path.read_text())
        else:
            reference_path = None
            reference = None
        report['sample']['human_reference'] = str(reference_path) if reference_path else None
        if report['model']['sha256'] != EXPECTED_MODEL_SHA256:
            raise ValueError('model_hash_mismatch')
        if sha256(FIXTURES / 'reference-model-config.json') != REFERENCE_MODEL_CONFIG_SHA256 or \
           sha256(FIXTURES / 'reference-preprocessor-config.json') != REFERENCE_PREPROCESS_CONFIG_SHA256:
            raise ValueError('reference_config_hash_mismatch')
        reference_preprocess = json.loads((FIXTURES / 'reference-preprocessor-config.json').read_text())
        if (reference_preprocess['size'] != {'height': 800, 'width': 800} or
            reference_preprocess['rescale_factor'] != 1/255 or
            reference_preprocess['image_mean'] != [0, 0, 0] or
            reference_preprocess['image_std'] != [1, 1, 1] or
            reference_preprocess['resample'] != 3):
            raise ValueError('reference_preprocess_contract_mismatch')
        revision = run_logged(['git', '-C', str(mnn), 'rev-parse', 'HEAD'], out / 'mnn-revision.log')
        if revision.returncode:
            raise ValueError('mnn_revision_unknown')
        report['environment']['mnn_source_commit'] = revision.stdout.strip()
        info_result = run_logged([str(info), str(args.model.resolve())], out / 'static-info.log', cwd=out)
        static_text = info_result.stdout
        if info_result.returncode or not all(token in static_text for token in
            ('[ image ]', '[ im_shape ]', '[ scale_factor ]', 'size: [ 1,3,800,800 ]',
             'size: [ 1,2 ]', 'Model Version: 3.6.1', 'Model bizCode: PPDocLayoutV3')):
            raise ValueError('static_input_contract_mismatch')
        report['checks']['static'] = 'passed'
        page_hw, input_path, comparison = preprocess(args.image, out, args.max_ref_abs)
        report['sample'].update({'hw': list(page_hw), 'input_sha256': sha256(input_path)})
        report['preprocess'] = comparison
        if not comparison['reference_pass']:
            report['checks']['reference_preprocess'] = 'failed'
            raise ValueError('reference_preprocess_alignment_failed')
        report['checks']['reference_preprocess'] = 'passed'
        binary = out / 'model_probe_layout_runner'
        build = run_logged(['c++', '-std=c++17', '-O2', '-I', str(mnn / 'include'),
            str(ROOT / 'scripts/model_probe_layout.cpp'), '-L', str(mnn / 'build'), '-lMNN',
            '-Wl,-rpath,' + str(mnn / 'build'), '-o', str(binary)], out / 'build.log')
        if build.returncode:
            raise ValueError('probe_build_failed')
        runs = {}
        for name, multiplier in (('normal', 1), ('scale_factor_half', 2)):
            prefix = out / name
            result = run_logged([str(binary), str(args.model.resolve()), str(input_path), str(prefix),
                                 str(page_hw[0]), str(page_hw[1]), str(multiplier)], out / (name + '.log'))
            runs[name] = {'exit_code': result.returncode, 'scale_factor':
                          [800/(page_hw[0]*multiplier), 800/(page_hw[1]*multiplier)],
                          'im_shape': [800, 800], 'log': name + '.log'}
            if result.returncode:
                raise ValueError(name + '_run_failed')
            version = result.stdout.split('MNN_VERSION=')[-1].split()[0]
            report['environment']['mnn_version'] = version
        report['runs'] = runs
        def load(prefix):
            boxes = np.fromfile(out / (prefix + '.fetch_name_0.bin'), dtype='<f4')
            count = np.fromfile(out / (prefix + '.fetch_name_1.bin'), dtype='<i4')
            masks = np.fromfile(out / (prefix + '.fetch_name_2.bin'), dtype='<i4')
            if boxes.size != 300*7 or count.size != 1 or masks.size != 300*200*200 or count[0] != 300:
                raise ValueError('output_contract_mismatch')
            return boxes.reshape(300, 7), masks.reshape(300, 200, 200)
        boxes, masks = load('normal')
        doubled, doubled_masks = load('scale_factor_half')
        report['checks']['runtime'] = 'passed'
        labels = json.loads((FIXTURES / 'reference-model-config.json').read_text())['id2label']
        if len(labels) != 25 or sorted(map(int, labels)) != list(range(25)):
            raise ValueError('label_mapping_mismatch')
        records = evaluate_candidates(boxes, masks, args.threshold, page_hw, labels)
        write_json(out / 'candidates.json', records)
        geometry = {'class_score_rank_max_abs': float(np.max(np.abs(boxes[:, [0,1,6]] - doubled[:, [0,1,6]]))),
                    'twice_box_max_abs': float(np.max(np.abs(boxes[:, 2:6]*2 - doubled[:, 2:6]))),
                    'masks_exactly_equal': bool(np.array_equal(masks, doubled_masks)),
                    'rank_unique': int(np.unique(boxes[:, 6]).size),
                    'rank_duplicate_count': int(len(boxes)-np.unique(boxes[:, 6]).size),
                    'rank_is_continuous': bool(np.array_equal(np.unique(boxes[:, 6]),
                        np.arange(int(boxes[:, 6].min()), int(boxes[:, 6].max())+1)))}
        write_json(out / 'geometry.json', geometry)
        if geometry['class_score_rank_max_abs'] > 1e-5 or geometry['twice_box_max_abs'] > .002 or not geometry['masks_exactly_equal']:
            raise ValueError('scale_factor_response_mismatch')
        selected = [record for record in records if record['selected']]
        write_json(out / 'selected-reference-order.json',
                   sorted(selected, key=lambda record: (record['rank'], record['candidate_id'])))
        mask_dir = out / 'page-masks'
        mask_dir.mkdir(exist_ok=True)
        overlay = cv2.cvtColor(np.asarray(Image.open(args.image).convert('RGB')), cv2.COLOR_RGB2BGR)
        mask_differences = []
        for record in selected:
            row = record['candidate_id']
            page_mask = map_mask_to_page(masks[row], record['box_xyxy'], page_hw)
            ref_page_mask = reference_page_mask(masks[row], record['box_xyxy'], page_hw)
            mask_differences.append(int(np.count_nonzero(page_mask != ref_page_mask)))
            cv2.imwrite(str(mask_dir / f'{row:03d}.png'), page_mask * 255)
            x0,y0,x1,y1 = [int(x) for x in record['box_xyxy']]
            cv2.rectangle(overlay, (x0,y0), (x1,y1), (0,0,255), 1)
            cv2.putText(overlay, f'{row}:{record["class_name"]}', (x0,max(8,y0-2)),
                        cv2.FONT_HERSHEY_SIMPLEX, .32, (0,0,255), 1)
        if any(mask_differences):
            raise ValueError('mask_reference_mapping_mismatch')
        write_json(out / 'mask-reference-comparison.json',
                   {'candidate_ids': [r['candidate_id'] for r in selected],
                    'different_pixels_by_candidate': mask_differences,
                    'max_different_pixels': max(mask_differences, default=0),
                    'reference': 'HF _extract_polygon_points_by_masks crop + INTER_NEAREST resize'})
        if reference:
            gt = [{'category': next(c['name'] for c in reference['categories'] if c['id']==ann['category_id']),
                   'box_xywh': ann['bbox'], 'best_detection_iou': max((iou(rec['box_xyxy'], ann['bbox'])
                   for rec in selected), default=0.0),
                   'fine_box_union_coverage': covered_fraction([rec['box_xyxy'] for rec in selected],
                       ann['bbox'], page_hw)} for ann in reference['annotations']]
            for annotation in reference['annotations']:
                x, y, w, h = annotation['bbox']
                cv2.rectangle(overlay, (x,y), (x+w,y+h), (255,0,0), 1)
        cv2.imwrite(str(out / 'overlay.jpg'), overlay)
        overlaps = sum(iou(a['box_xyxy'], [b['box_xyxy'][0], b['box_xyxy'][1],
                    b['box_xyxy'][2]-b['box_xyxy'][0], b['box_xyxy'][3]-b['box_xyxy'][1]]) > .5
                    for index,a in enumerate(selected) for b in selected[index+1:]
                    if a['class_id'] == b['class_id'])
        if reference:
            write_json(out / 'business-reference-comparison.json', {'reference_schema': 'HiLEx hierarchical question blocks',
                'model_schema': 'PP-DocLayoutV3 visual elements; categories are not directly equivalent',
                'annotations': gt, 'same_class_box_pairs_iou_gt_0_5': overlaps,
                'quality_status': 'reviewable_only_no_class_accuracy_claim'})
        report['outputs'] = {'candidate_count': len(records), 'selected_count': len(selected),
            'selected_reference_order': 'selected-reference-order.json',
            'rejected_by_reason': {reason: sum(reason in rec['reasons'] for rec in records)
            for reason in ('below_score_threshold','outside_page','degenerate_box','unknown_class','nonintegral_rank')},
            'raw_masks': ['normal.fetch_name_2.bin','scale_factor_half.fetch_name_2.bin'],
            'geometry': geometry, 'overlay': 'overlay.jpg',
            'mask_reference_max_different_pixels': max(mask_differences, default=0),
            'business_reference': 'business-reference-comparison.json' if reference else None}
        report['overall'] = 'diagnostic_passed'
    except (OSError, ValueError, RuntimeError, cv2.error) as error:
        report['errors'].append(str(error))
    finally:
        save()
    return report['exit_code']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model', type=Path, default=ROOT / 'models/doclayout/PP-DocLayoutV3.mnn')
    parser.add_argument('--image', type=Path, default=FIXTURES / 'exam-jee-346.jpg')
    parser.add_argument('--mnn', type=Path, default=ROOT.parent / 'MNN')
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--reference', type=Path,
                        help='仅接受带 image_sha256 且与 --image 匹配的人工参考 JSON')
    parser.add_argument('--threshold', type=float, default=.5)
    parser.add_argument('--max-ref-abs', type=float, default=1/255 + 1e-7)
    args = parser.parse_args()
    if not math.isfinite(args.threshold) or not 0 <= args.threshold <= 1 or not math.isfinite(args.max_ref_abs) or args.max_ref_abs < 0:
        parser.error('threshold must be in [0,1] and max-ref-abs must be nonnegative')
    return run(args)


if __name__ == '__main__':
    sys.exit(main())
