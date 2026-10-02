"""执行冻结 PaddleX 后处理，旁路记录阶段来源；不重写官方算法。"""
import argparse
import ast
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import sys
from typing import Dict, List, Optional, Tuple, Union
from urllib.request import urlopen

import cv2
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / 'docs/issue-28/evidence/reference-sources.json'


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def save(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2,
                                   default=lambda x: x.tolist() if isinstance(x, np.ndarray) else x.item()) + '\n')


def load_reference(directory):
    """校验完整文件，再仅载入函数/类及一个常量；不导入 Paddle 运行时。"""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    manifest = json.loads(SOURCES.read_text())
    for name, spec in manifest.items():
        path = directory / name
        if not path.exists():
            data = urlopen(spec['url'], timeout=60).read()
            if hashlib.sha256(data).hexdigest() != spec['sha256']:
                raise ValueError(f'冻结参照下载哈希不符：{name}')
            path.write_bytes(data)
        if sha(path) != spec['sha256']:
            raise ValueError(f'冻结参照文件哈希不符：{name}')
    env = dict(np=np, ndarray=np.ndarray, cv2=cv2, deepcopy=deepcopy, Dict=Dict,
               List=List, Optional=Optional, Tuple=Tuple, Union=Union,
               Boxes=list, Number=Union[int, float])
    for name in ('detection.py', 'layout.py', 'pipeline_utils.py'):
        tree = ast.parse((directory / name).read_text())
        nodes = []
        pipeline_names = {'make_valid', 'calculate_polygon_overlap_ratio',
                          '_compute_pairwise_overlap_small', 'filter_overlap_boxes'}
        for node in tree.body:
            if isinstance(node, ast.FunctionDef) and (name != 'pipeline_utils.py' or node.name in pipeline_names):
                nodes.append(node)
            elif isinstance(node, ast.ClassDef) and node.name == 'LayoutAnalysisProcess':
                nodes.append(node)
            elif isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'SKIP_ORDER_LABELS'
                                                     for t in node.targets):
                nodes.append(node)
        for node in ast.walk(ast.Module(body=nodes, type_ignores=[])):
            if isinstance(node, (ast.FunctionDef, ast.ClassDef)):
                node.decorator_list = []
        exec(compile(ast.Module(body=nodes, type_ignores=[]), str(directory / name), 'exec'), env)
    # 此冻结 yml 的 label_list 是简单列表，不引入额外 YAML 执行器。
    metadata = (directory / 'inference.yml').read_text()
    labels = metadata.split('label_list:\n', 1)[1].split('Hpi:', 1)[0]
    labels = [line[2:].strip() for line in labels.splitlines() if line.startswith('- ')]
    if len(labels) != 25:
        raise ValueError('官方模型标签契约不是 25 类')
    return env, labels


class StageTrace:
    """用行事件观察官方 apply 的局部数组，不改参数、排序或返回值。"""
    def __init__(self, process, rows, masks):
        self.code = process.apply.__func__.__code__
        tree = ast.parse(Path(self.code.co_filename).read_text())
        method = next(n for n in ast.walk(tree) if isinstance(n, ast.FunctionDef) and n.name == 'apply')
        markers = {'layout_nms': 'score', 'layout_merge_bboxes_mode': 'large_image',
                   'boxes.size == 0': 'containment',
                   'polygon_points is None and masks is not None': 'rank',
                   'layout_unclip_ratio': 'mask_geometry', 'boxes.shape[1] == 6': 'unclip'}
        self.lines = {}
        for node in method.body:
            if isinstance(node, ast.If) and ast.unparse(node.test) in markers:
                self.lines[node.lineno] = markers[ast.unparse(node.test)]
            if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'filter_large_image'
                                                   for t in node.targets):
                self.lines[node.lineno] = 'nms'
        if len(self.lines) != 7:
            raise ValueError('官方 apply 阶段定位不完整')
        self.previous = rows.copy()
        self.previous[:, 2:6] = np.round(self.previous[:, 2:6])
        self.ids = list(range(len(rows)))
        self.mask_hashes = [hashlib.sha256(m.tobytes()).hexdigest() for m in masks]
        self.stages = {}
        self.removed = {}

    def __call__(self, frame, event, arg):
        if frame.f_code is not self.code:
            return None
        if event != 'line' or frame.f_lineno not in self.lines:
            return self
        stage = self.lines[frame.f_lineno]
        rows = frame.f_locals['boxes']
        if stage == 'unclip':
            ids = self.ids[:]  # 官方扩框不改变候选数量/顺序。
        else:
            ids = []
            for row in rows:
                matches = [self.ids[i] for i, old in enumerate(self.previous)
                           if np.array_equal(old[:len(row)], row)]
                if len(matches) != 1:
                    raise ValueError('候选来源不唯一；拒绝用 class/score 猜测来源')
                ids.append(matches[0])
        for dropped in set(self.ids) - set(ids):
            self.removed[dropped] = stage
        masks = frame.f_locals['masks']
        mask_rows = None
        if masks is not None:
            if len(masks) != len(ids):
                raise ValueError('官方 mask 数量与候选错位')
            for index, mask in zip(ids, masks):
                if hashlib.sha256(np.asarray(mask).tobytes()).hexdigest() != self.mask_hashes[index]:
                    raise ValueError('官方 mask 与原始候选行不对应')
            mask_rows = ids[:]
        self.stages[stage] = dict(candidate_ids=ids, boxes=rows.tolist(), mask_rows=mask_rows)
        self.previous, self.ids = rows.copy(), ids
        return self


class OuterTrace:
    """观察实际执行的删除分支；原因和关联框不靠重新计算重叠推断。"""
    def __init__(self, function, boxes):
        self.code = function.__code__
        self.original_ids = {box['candidate_id'] for box in boxes}
        self.removals = {}
        self.lines = {}
        tree = ast.parse(Path(self.code.co_filename).read_text())
        function_node = next(n for n in tree.body if isinstance(n, ast.FunctionDef)
                             and n.name == 'filter_overlap_boxes')

        def visit(node, reason=None):
            if isinstance(node, ast.If):
                condition = ast.unparse(node.test)
                if condition == 'not boxes':
                    self.reference_line = node.lineno
                if condition == 'widths[i] < 6 or heights[i] < 6':
                    reason = 'short_box'
                elif condition == 'overlap_ratio > 0.5':
                    reason = 'inline_formula_overlap'
                elif condition == 'areas[i] >= areas[j]':
                    reason = 'smaller_overlapping_box'
            if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) and \
                    ast.unparse(node.func) == 'dropped_indexes.add':
                index = ast.unparse(node.args[0])
                if reason is None or index not in ('i', 'j'):
                    raise ValueError('官方外层删除分支定位不完整')
                self.lines[node.lineno] = (reason, index)
            for child in ast.iter_child_nodes(node):
                visit(child, reason)
        visit(function_node)
        if len(self.lines) != 5:
            raise ValueError('官方外层删除分支数量变化')

    def __call__(self, frame, event, arg):
        if frame.f_code is not self.code:
            return None
        if event == 'line':
            values = frame.f_locals
            if frame.f_lineno == self.reference_line:
                for index in self.original_ids - {b['candidate_id'] for b in values['boxes']}:
                    self.removals[index] = dict(reason='reference_label', related_candidate_id=None)
            if frame.f_lineno in self.lines:
                reason, index_name = self.lines[frame.f_lineno]
                candidate = values['boxes'][values[index_name]]['candidate_id']
                other = None if reason == 'short_box' else \
                    values['boxes'][values['j' if index_name == 'i' else 'i']]['candidate_id']
                self.removals[candidate] = dict(reason=reason, related_candidate_id=other)
        return self


def run_reference(env, labels, rows, masks, size, threshold, mode, unclip=(1., 1.)):
    rows = np.asarray(rows, dtype=np.float32).reshape(-1, 7)
    masks = np.asarray(masks, dtype=np.int32)
    if masks.shape != (len(rows), 200, 200) or not np.isfinite(rows).all():
        raise ValueError('候选或 mask 形状/有限数契约错误')
    if any(int(c) != c or not 0 <= c < len(labels) for c in rows[:, 0]):
        raise ValueError('官方标签范围外的候选须单独记录，不能静默丢弃')
    process = env['LayoutAnalysisProcess'](labels, [800, 800])
    trace = StageTrace(process, rows, masks)
    previous_trace = sys.gettrace()
    try:
        sys.settrace(trace)
        structured = process.apply(rows.copy(), tuple(size), float(threshold), True, tuple(unclip),
                                   {i: 'large' if i in (3, 5, 6, 15, 17) else 'union' for i in range(25)},
                                   masks=masks.copy(), layout_shape_mode=mode)
    finally:
        sys.settrace(previous_trace)
    structured = list(structured)
    valid_ids = []
    for index, row in zip(trace.ids, trace.previous):
        x0, y0, x1, y1 = row[2:6]
        if int(min(size[0], x1)) > int(max(0, x0)) and int(min(size[1], y1)) > int(max(0, y0)):
            valid_ids.append(index)
        else:
            trace.removed[index] = 'structure_boundary'
    if len(valid_ids) != len(structured):
        raise ValueError('结构化结果与有效候选来源不符')
    for index, box in zip(valid_ids, structured):
        box.update(candidate_id=index, mask_row=index, rank=int(rows[index, 6]))
    # 独立模型：apply → filter_boxes → update_order_index。
    standalone = env['update_order_index'](env['filter_boxes'](deepcopy(structured), mode), env['SKIP_ORDER_LABELS'])
    # VL 管线：模型 filter_overlap_boxes=False → update_order_index → 管线外层过滤。
    model_output = env['update_order_index'](deepcopy(structured), env['SKIP_ORDER_LABELS'])
    outer_trace = OuterTrace(env['filter_overlap_boxes'], model_output)
    try:
        sys.settrace(outer_trace)
        pipeline = env['filter_overlap_boxes']({'boxes': model_output}, mode)['boxes']
    finally:
        sys.settrace(previous_trace)
    survivors = {box['candidate_id'] for box in pipeline}
    if set(outer_trace.removals) != set(valid_ids) - survivors:
        raise ValueError('外层删除原因与实际输出不一致')
    for index in set(valid_ids) - survivors:
        trace.removed[index] = 'outer_overlap'
    candidates = [dict(candidate_id=i, class_id=int(row[0]), original_bbox=row[2:6].tolist(),
                       rank=int(row[6]), mask_row=i, mask_sha256=trace.mask_hashes[i],
                       mask_nonzero=int(np.count_nonzero(masks[i])), selected=i in survivors,
                       removed_at=trace.removed.get(i),
                       removal_reason=outer_trace.removals.get(i, {}).get('reason', trace.removed.get(i)),
                       related_candidate_id=outer_trace.removals.get(i, {}).get('related_candidate_id'))
                  for i, row in enumerate(rows)]
    return dict(stages=trace.stages, candidates=candidates, model_output=model_output,
                pipeline_output=pipeline, standalone_output=standalone)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--reference', type=Path, default=ROOT / '.scratch/issue28-reference')
    args = parser.parse_args()
    value = json.loads(args.input.read_text())
    env, labels = load_reference(args.reference)
    rows = np.asarray(value['rows'], dtype=np.float32).reshape(-1, 7)
    masks = np.zeros((len(rows), 200, 200), dtype=np.int32)
    for i, (x0, y0, x1, y1) in enumerate(value.get('mask_rectangles', [])):
        masks[i, y0:y1, x0:x1] = 1
    report = dict(labels=labels, input_sha256=sha(args.input), sources=json.loads(SOURCES.read_text()))
    for mode in ('rect', 'auto'):
        report[mode] = run_reference(env, labels, rows, masks, value['size'],
                                     value.get('threshold', .3), mode, value.get('unclip', [1., 1.]))
    save(args.out, report)


if __name__ == '__main__':
    main()
