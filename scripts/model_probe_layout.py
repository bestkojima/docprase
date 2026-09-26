#!/usr/bin/env python3
"""PP-DocLayoutV3-MNN 的单页可复核诊断入口。只生成证据，不充当文档解析器。"""
import argparse
import gzip
import hashlib
import json
import math
import platform
import shutil
import subprocess
import sys
from pathlib import Path
from unittest.mock import patch

import cv2
import numpy as np
from PIL import Image
import torch

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / 'tests/fixtures/layout'
EXPECTED_MODEL_SHA256 = '5f1a43441d70f6843012b47eb294bed7edd3d0ef2344f0074700a38cb2e29c67'
EXAM_SAMPLE_SHA256 = 'e8d587b83baade2dbdb3ad3333cfe8bc9a7d9cbf489de4db961058b23343dade'
REFERENCE_TRANSFORMERS_COMMIT = '27166ea03f12c940f23176a904ab1d2ff1a3dcbb'
REFERENCE_PROCESSOR_SHA256 = '5b064fa7383dda12b3550448eae77d4f627a102c25e8b4db25d99e85fba4abc6'
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


def write_gzip(source, target):
    with open(source, 'rb') as source_file, open(target, 'wb') as compressed_file:
        with gzip.GzipFile(fileobj=compressed_file, mode='wb', mtime=0) as archive:
            shutil.copyfileobj(source_file, archive)


def compare_arrays(a, b):
    difference = np.abs(a.astype(np.float32) - b.astype(np.float32))
    return {'max_abs': float(difference.max()), 'mean_abs': float(difference.mean()),
            'p99_abs': float(np.percentile(difference, 99)), 'different_fraction': float(np.mean(difference != 0))}


def preprocess(image_path, out, max_ref_abs, backend, processor_config):
    from torchvision.transforms.v2 import functional as tvf
    from transformers.models.pp_doclayout_v3 import image_processing_pp_doclayout_v3 as reference_module
    from transformers.models.pp_doclayout_v3.image_processing_pp_doclayout_v3 import PPDocLayoutV3ImageProcessor
    if sha256(reference_module.__file__) != REFERENCE_PROCESSOR_SHA256:
        raise ValueError('reference_processor_source_hash_mismatch')

    source = Image.open(image_path).convert('RGB')
    rgb = np.asarray(source)
    h, w = rgb.shape[:2]
    processor = PPDocLayoutV3ImageProcessor(**processor_config)
    reference = processor(images=source, return_tensors='pt')['pixel_values'].numpy()[0]
    # 候选路径独立调用 torchvision 公共 resize；参考是锁定的完整 HF 处理器。
    tensor = torch.from_numpy(rgb.copy()).permute(2, 0, 1)
    tv_resized = tvf.resize(tensor, [800, 800], interpolation=tvf.InterpolationMode.BICUBIC,
                            antialias=False)
    tv_tensor = np.ascontiguousarray(tv_resized.numpy().astype(np.float32) / 255.0)
    cv_resized = cv2.resize(rgb, (800, 800), interpolation=cv2.INTER_CUBIC)
    pillow_resized = np.asarray(source.resize((800, 800), Image.Resampling.BICUBIC))
    cv_tensor = np.ascontiguousarray(cv_resized.transpose(2, 0, 1).astype(np.float32) / 255.0)
    pillow_tensor = np.ascontiguousarray(pillow_resized.transpose(2, 0, 1).astype(np.float32) / 255.0)
    candidates = {'torchvision': tv_tensor, 'opencv': cv_tensor, 'pillow': pillow_tensor}
    candidate = candidates[backend]
    input_path = out / 'image.f32'
    candidate.tofile(input_path)
    comparison = {'reference': 'PPDocLayoutV3ImageProcessor(images=PIL RGB, return_tensors=pt)',
                  'candidate': backend, 'color': 'RGB', 'rescale_factor': 1/255,
                  'tolerance_max_abs': max_ref_abs,
                  'candidate_vs_reference': compare_arrays(candidate, reference),
                  'opencv_vs_reference': compare_arrays(cv_tensor, reference),
                  'pillow_vs_reference': compare_arrays(pillow_tensor, reference),
                  'reference_pass': compare_arrays(candidate, reference)['max_abs'] <= max_ref_abs}
    write_json(out / 'preprocess-comparison.json', comparison)
    return (h, w), input_path, comparison, processor


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


def compare_mask_with_reference(processor, mask, box, page_hw):
    """调用锁定 HF 处理器，并截取其实际 resize patch 与页面映射逐像素比较。"""
    height, width = page_hw
    original_resize = cv2.resize
    patches = []

    def capture_resize(*args, **kwargs):
        resized = original_resize(*args, **kwargs)
        patches.append(resized.copy())
        return resized

    with patch.object(cv2, 'resize', side_effect=capture_resize):
        polygon = processor._extract_polygon_points_by_masks(
            np.asarray([box], dtype=np.float32), mask[np.newaxis], [800 / width, 800 / height])[0]
    reference_page = np.zeros((height, width), dtype=np.uint8)
    x0, y0, x1, y1 = [int(v) for v in box]
    px0, px1 = max(0, x0), min(width, x1)
    py0, py1 = max(0, y0), min(height, y1)
    if patches and px1 > px0 and py1 > py0:
        reference_page[py0:py1, px0:px1] = patches[0][py0-y0:py1-y0, px0-x0:px1-x0]
    actual = map_mask_to_page(mask, box, page_hw)
    return int(np.count_nonzero(actual != reference_page)), np.asarray(polygon).tolist() if polygon is not None else None


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
        def as_text(value):
            return value.decode(errors='replace') if isinstance(value, bytes) else (value or '')
        result = subprocess.CompletedProcess(command, 124, as_text(error.stdout),
                                             'timeout_after_120_seconds\n' + as_text(error.stderr))
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
                            'processor_source_sha256': REFERENCE_PROCESSOR_SHA256,
                            'model_config_revision': REFERENCE_MODEL_REVISION,
                            'model_config_sha256': REFERENCE_MODEL_CONFIG_SHA256,
                            'preprocessor_config_sha256': REFERENCE_PREPROCESS_CONFIG_SHA256},
              'parameters': {'threshold': args.threshold, 'max_ref_abs': args.max_ref_abs,
                             'preprocess': args.preprocess, 'backend': 'CPU',
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
        page_hw, input_path, comparison, processor = preprocess(
            args.image, out, args.max_ref_abs, args.preprocess, reference_preprocess)
        import torchvision
        import transformers
        report['environment']['torchvision'] = torchvision.__version__
        report['environment']['transformers'] = transformers.__version__
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
        polygons = []
        for record in selected:
            row = record['candidate_id']
            page_mask = map_mask_to_page(masks[row], record['box_xyxy'], page_hw)
            difference, polygon = compare_mask_with_reference(
                processor, masks[row], record['box_xyxy'], page_hw)
            mask_differences.append(difference)
            polygons.append({'candidate_id': row, 'polygon_xy': polygon})
            cv2.imwrite(str(mask_dir / f'{row:03d}.png'), page_mask * 255)
            x0,y0,x1,y1 = [int(x) for x in record['box_xyxy']]
            cv2.rectangle(overlay, (x0,y0), (x1,y1), (0,0,255), 1)
            cv2.putText(overlay, f'{row}:{record["class_name"]}', (x0,max(8,y0-2)),
                        cv2.FONT_HERSHEY_SIMPLEX, .32, (0,0,255), 1)
        write_json(out / 'mask-reference-comparison.json',
                   {'candidate_ids': [r['candidate_id'] for r in selected],
                    'different_pixels_by_candidate': mask_differences,
                    'max_different_pixels': max(mask_differences, default=0),
                    'reference': 'actual PPDocLayoutV3ImageProcessor._extract_polygon_points_by_masks',
                    'polygons': polygons})
        if any(mask_differences):
            raise ValueError('mask_reference_mapping_mismatch')
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
            'raw_masks': ['normal.masks.i32.gz','scale-factor-half.masks.i32.gz'],
            'runtime_mask_files': ['normal.fetch_name_2.bin','scale_factor_half.fetch_name_2.bin'],
            'geometry': geometry, 'overlay': 'overlay.jpg',
            'mask_reference_max_different_pixels': max(mask_differences, default=0),
            'business_reference': 'business-reference-comparison.json' if reference else None}
        for source_name, archive_name in (
            ('image.f32', 'image.f32.gz'),
            ('normal.fetch_name_0.bin', 'normal.candidates.f32.gz'),
            ('normal.fetch_name_1.bin', 'normal.count.i32.gz'),
            ('normal.fetch_name_2.bin', 'normal.masks.i32.gz'),
            ('scale_factor_half.fetch_name_0.bin', 'scale-factor-half.candidates.f32.gz'),
            ('scale_factor_half.fetch_name_1.bin', 'scale-factor-half.count.i32.gz'),
            ('scale_factor_half.fetch_name_2.bin', 'scale-factor-half.masks.i32.gz'),
        ):
            write_gzip(out / source_name, out / archive_name)
        report['overall'] = 'diagnostic_passed'
    except (OSError, ValueError, RuntimeError, ImportError, cv2.error) as error:
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
    parser.add_argument('--preprocess', choices=('torchvision', 'opencv', 'pillow'), default='torchvision')
    parser.add_argument('--max-ref-abs', type=float, default=1e-7)
    args = parser.parse_args()
    if not math.isfinite(args.threshold) or not 0 <= args.threshold <= 1 or not math.isfinite(args.max_ref_abs) or args.max_ref_abs < 0:
        parser.error('threshold must be in [0,1] and max-ref-abs must be nonnegative')
    return run(args)


if __name__ == '__main__':
    sys.exit(main())
