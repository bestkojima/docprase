"""核验 #26 冻结生产作业的结构、资源、重新导出及共同 GT 顺序；不声明 OCR 质量达标。"""
import argparse
from collections import Counter
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile

import jsonschema
from PIL import Image

from issue17_baseline import TEXT_CATEGORIES, match
from issue20_runtime import runtime_sources
from issue26_annotations import block_candidates

ROOT = Path(__file__).resolve().parents[1]
UNCERTAIN = '图注与图片的对应关系尚未确认，请核对原图。'


def read(path):
    return json.loads(path.read_text())


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')


def sha(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def require(value, message):
    if not value:
        raise ValueError(message)


def covers(parent, child):
    return parent[0] <= child[0] and parent[1] <= child[1] and \
        parent[2] >= child[2] and parent[3] >= child[3]


def paired_refs(annotation, document):
    """沿用原一对一几何配对口径；全文内容和多 GT 对齐留给 #27。"""
    result = {}
    ids = block_candidates(document['pages'][0])
    for kind, categories, output_type, threshold in (
        ('text', TEXT_CATEGORIES, 'text', .5),
        ('independent_formula', {'equation_isolated'}, 'formula', .8),
        ('table', {'table'}, 'table', .5),
        ('figure', {'figure'}, 'image', .5),
    ):
        expected = [a for a in annotation['layout_dets']
                    if not a.get('ignore') and a['category_type'] in categories]
        predicted = [b for b in document['pages'][0]['blocks'] if b['type'] == output_type]
        pairs, _ = match(expected, predicted, threshold)
        for index, (pred_index, _) in pairs.items():
            ref, block = expected[index], predicted[pred_index]
            if isinstance(ref.get('order'), int):
                result[(kind, ref['anno_id'])] = dict(order=ref['order'], block_id=block['id'],
                                                    candidates=sorted(ids[block['id']]))
    return result


def common_order(annotation, before, after):
    previous, current = paired_refs(annotation, before), paired_refs(annotation, after)
    keys = previous.keys() & current.keys()
    common = sorted((key for key in keys if previous[key]['candidates'] == current[key]['candidates']),
                    key=lambda key: (previous[key]['order'], str(key)))
    old = {bid: i for i, bid in enumerate(before['pages'][0]['reading_order'])}
    new = {bid: i for i, bid in enumerate(after['pages'][0]['reading_order'])}
    fixed, added, total, was_correct, now_correct = [], [], 0, 0, 0
    for index, a in enumerate(common):
        for b in common[index + 1:]:
            if previous[a]['order'] == previous[b]['order'] or \
               previous[a]['block_id'] == previous[b]['block_id'] or \
               current[a]['block_id'] == current[b]['block_id']:
                continue
            was = old[previous[a]['block_id']] < old[previous[b]['block_id']]
            now = new[current[a]['block_id']] < new[current[b]['block_id']]
            total += 1
            was_correct += was
            now_correct += now
            row = dict(annotations=[list(a), list(b)],
                       before_blocks=[previous[a]['block_id'], previous[b]['block_id']],
                       after_blocks=[current[a]['block_id'], current[b]['block_id']])
            if not was and now:
                fixed.append(row)
            elif was and not now:
                added.append(row)
    return dict(basis='same_original_GT_pairs_with_same_source_candidate_ownership_on_both_sides',
                matched_refs_before=len(previous), matched_refs_after=len(current),
                shared_identity_refs=len(common), changed_identity_refs=len(keys)-len(common),
                denominator=total, before_correct=was_correct, after_correct=now_correct,
                fixed=fixed, newly_wrong=added)


def verify_page(spec, annotation, args, freeze, environment):
    folder = args.run/spec['id']
    job = folder/'job'
    receipt = read(folder/'command.json')
    require(receipt.get('returncode') == 0 and receipt.get('candidate_intact'), '作业未成功或候选改变')
    require(receipt['execution'] == freeze['execution'], '执行来源不符')
    require(receipt['provenance']['freeze_sha256'] == sha(args.run/'freeze.json'), '冻结记录不符')
    require(receipt['provenance']['source_sha256'] == freeze['source_sha256'], '源码凭据不符')
    require(receipt['provenance']['runtime'] == freeze['runtime'], '运行时凭据不符')
    image = args.data/spec['image_path']
    require(sha(image) == spec['image_sha256'] == receipt['input_sha256'], '完整原图哈希不符')
    document = read(job/'document.json')
    version = document['schema_version']
    require(version in ('1.8', '1.9'), '结构验收只支持 DocumentIR 1.8/1.9')
    jsonschema.Draft202012Validator(read(ROOT/f'docs/issue-26/document-ir-{version}-image.schema.json')).validate(document)
    page = document['pages'][0]
    with Image.open(image) as original:
        require(list(original.size) == page['raster_size'], '未使用完整原页')
    plan = page['structure_plan']
    blocks = {b['id']: b for b in page['blocks']}
    regions = {r['id']: r for r in page['regions']}
    layouts = {b['id']: b for b in page['layout_blocks']}
    require(plan['stage'] == 'before_recognition', '计划阶段错误')
    require(plan['block_order'] == page['reading_order'] == [b['id'] for b in page['blocks']], '导出顺序不同')
    require(len(plan['block_order']) == len(blocks), '块被重复调度')
    require(set(plan['region_order']) == regions.keys() and len(plan['region_order']) == len(regions),
            'Region 调度未覆盖或重复')
    require([rid for b in page['blocks'] for rid in b['source_region_ids']] == plan['region_order'],
            '块与 Region 顺序不同')
    manifest = read(job/'run-manifest.json')
    execution = read(job/'execution-plan.json')
    require(manifest['backend_id'] == 'mnn:pp-doclayout-v3+ovisocr2' and
            manifest['actual_backend'].startswith('MNN/'), '不是生产推理')
    require(manifest['config_hash'] == execution['config_hash'], '运行配置不符')
    require(execution['effective_config'] == read(args.run/'.runtime/config.json'), '未使用冻结配置')
    request_order = plan['recognition_order'] if version == '1.9' else plan['region_order']
    require([r['request_id'] for r in manifest['regions']] == ['req'+rid for rid in request_order],
            '实际处理调度不同于识别前计划')
    if version == '1.9':
        require(request_order == [rid for b in page['blocks'] if b['type'] in ('text', 'formula', 'table')
                                  for rid in b['source_region_ids']], '识别任务包含图片资源或遗漏 OCR 区域')
        for block in blocks.values():
            require(all(regions[rid]['recognition_type'] == block['type'] for rid in block['source_region_ids']),
                    '统一区域类型与输出块不一致')
            if block['type'] == 'image':
                require(block['status'] == 'ok' and block['error'] is None and
                        block['provenance']['recognition']['attempts'] == [] and
                        block['provenance']['assessment']['reason'] == 'resource_saved', '图片资源被识别或误标失败')
    selected = {c['candidate_id']: c for c in document['layout_diagnostics']['candidates'] if c['selected']}
    require(selected.keys() == {b['candidate_id'] for b in layouts.values()}, '选中候选遗漏')
    source_ids = [lid for region in regions.values() for lid in region['source_layout_block_ids']]
    require(len(source_ids) == len(set(source_ids)) and set(source_ids) == layouts.keys(), '来源未唯一归属')
    owners = {lid: block['id'] for block in blocks.values() for rid in block['source_region_ids']
              for lid in regions[rid]['source_layout_block_ids']}
    for region in regions.values():
        for lid in region['source_layout_block_ids']:
            require(covers(region['bbox'], layouts[lid]['bbox']), f'{lid}: 父裁图未覆盖来源')
    for relation in page['relations']:
        if relation['type'] == 'content_owned_by':
            require(owners[relation['source_layout_block_id']] == relation['owner_block_id'], '归属关系不符')
    for layout in layouts.values():
        candidate = selected[layout['candidate_id']]
        # 两处已有序列化分别保留六位小数/九位有效数字；仅容纳其舍入误差。
        same_float_box = all(math.isclose(a, b, rel_tol=1e-8, abs_tol=1e-6)
                             for a, b in zip(layout['original_bbox'], candidate['original_bbox']))
        require(layout['bbox'] == candidate['crop_bbox'] and layout['candidate_rank'] == candidate['rank'] and
                same_float_box and layout['mask_row'] == candidate['mask_row'],
                '布局候选几何或原始证据被覆盖')
    md = (job/'document.md').read_text()
    require(md.count(UNCERTAIN) == len(plan['ambiguous_caption_ids']), '不确定关联提示缺失')
    for block in blocks.values():
        resource = block['content']['resource']
        if block['type'] == 'image':
            require(f'![插图]({resource})' in md, 'Markdown 未引用图片地址')
        if block['status'] != 'ok' or block['id'] in plan['ambiguous_caption_ids']:
            require(f']({resource})' in md, '非成功/不确定块未保留原图')
    for resource in document['resources']:
        path = (job/resource['path']).resolve()
        require(path.is_relative_to(job.resolve()) and path.is_file(), '资源缺失或越界')
        with Image.open(path) as crop:
            require(list(crop.size) == [resource['width'], resource['height']], '裁图尺寸错误')
    require(all((job/p).is_file() for p in document['layout_diagnostics']['raw_tensor_assets'].values()),
            '原始张量证据缺失')
    target = folder/('acceptance-partial-reexport' if args.only else 'acceptance-reexport')
    if target.exists():
        # 核验可重复执行，但不覆盖已经保存的重新导出产物。
        target = Path(tempfile.mkdtemp(prefix='acceptance-reexport-', dir=folder))/'job'
    command = [str(args.run/'.runtime/dococr_cli'), '--reexport', str(job/'document.json'),
               '--asset-root', str(job), '--out', str(target)]
    result = subprocess.run(command, cwd=ROOT, env=environment, capture_output=True, text=True)
    (folder/'acceptance-reexport.log').write_text(result.stdout+result.stderr)
    require(result.returncode == 0, '生产重新导出失败：'+result.stderr)
    require((job/'document.md').read_bytes() == (target/'document.md').read_bytes(), 'Markdown 重新导出不等价')
    require(document == read(target/'document.json'), '重新导出改变 IR')
    require(all(sha(job/r['path']) == sha(target/r['path']) for r in document['resources']), '重新导出改变裁图')
    before = read(args.before/spec['id']/'baseline/document.json')
    old_page = before['pages'][0]
    require(before['layout_diagnostics']['candidates'] == document['layout_diagnostics']['candidates'],
            '真实新推理与冻结候选参照不同，需解释差异')
    identities = lambda value: {r['id']: (r['bbox'], r['source_layout_block_ids']) for r in value['regions']}
    require(identities(old_page) == identities(page), 'Region 身份、裁图或所有权与参照不同')
    old_blocks = {b['id']: b for b in old_page['blocks']}
    require(all(b['content']['resource'] == old_blocks[bid]['content']['resource'] and
                sha(job/b['content']['resource']) ==
                sha(args.before/spec['id']/'baseline'/old_blocks[bid]['content']['resource'])
                for bid, b in blocks.items()), '真实新作业改变原页裁图资源')
    orders = common_order(annotation, before, document)
    require(not orders['newly_wrong'], '共同 GT 对出现新增错序，需核验')
    previous_comparison = None
    if args.previous:
        previous_job = args.previous/spec['id']/'job'
        previous_document = read(previous_job/'document.json')
        previous_page = previous_document['pages'][0]
        require(previous_document['layout_diagnostics']['candidates'] == document['layout_diagnostics']['candidates'] and
                identities(previous_page) == identities(page), '与上一候选的来源、裁图或归属不同')
        previous_blocks = {b['id']: b for b in previous_page['blocks']}
        require(previous_blocks.keys() == blocks.keys() and
                all(previous_blocks[bid]['type'] == b['type'] for bid, b in blocks.items()),
                '统一映射改变了实际内容区域类型或数量')
        previous_orders = common_order(annotation, previous_document, document)
        require(not previous_orders['newly_wrong'], '相对已通过的上一候选出现新增 GT 错序')
        caption_pairs = lambda p: {(c['image_block_id'], c['caption_block_id'])
                                  for c in p['structure_plan']['captions']}
        old_pairs, new_pairs = caption_pairs(previous_page), caption_pairs(page)
        previous_comparison = dict(document_sha256=sha(previous_job/'document.json'),
            schema_version=previous_document['schema_version'], common_GT_order=previous_orders,
            same_reading_order=previous_page['reading_order'] == page['reading_order'],
            added_captions=sorted(new_pairs-old_pairs), removed_captions=sorted(old_pairs-new_pairs),
            changed_OCR_block_ids=[bid for bid, b in blocks.items() if b['type'] in ('text', 'formula', 'table') and
                (b['content'], b['provenance']['raw_output'], b['status']) !=
                (previous_blocks[bid]['content'], previous_blocks[bid]['provenance']['raw_output'], previous_blocks[bid]['status'])])
    ids = block_candidates(page)
    labels = [{**item, 'image_candidates': ids[item['image_block_id']],
               'caption_candidates': ids[item['caption_block_id']],
               'text': blocks[item['caption_block_id']]['content']['text'],
               'status': blocks[item['caption_block_id']]['status']} for item in plan['captions']]
    if version == '1.9' and spec['id'] in ('odb-07', 'odb-08'):
        image_id, caption_id = (9, 10) if spec['id'] == 'odb-07' else (20, 42)
        require(any(image_id in item['image_candidates'] and caption_id in item['caption_candidates']
                    for item in labels), '目标单图文字关联缺失')
    for item in plan['captions']:
        image_block, caption = blocks[item['image_block_id']], blocks[item['caption_block_id']]
        if caption['status'] == 'ok':
            require(f'![插图]({image_block["content"]["resource"]})\n\n{caption["content"]["text"]}' in md,
                    'Markdown 图片与图注没有相邻输出')
    if spec['id'] == 'odb-03':
        expected = [([8, 5, 6, 13], [32, 27, 36, 33]), ([4, 1, 0, 2], [26, 31, 25, 30])]
        require(len(plan['groups']) == 2 and len(labels) == 8, '第17/18题分组或八个绑定缺失')
        for group, (images, captions) in zip(plan['groups'], expected):
            require([ids[i['image_block_id']] for i in group['items']] == [[c] for c in images] and
                    [ids[i['caption_block_id']] for i in group['items']] == [[c] for c in captions],
                    '第17/18题图片与原图标签未按 A/B/C/D 位置绑定')
        require(all(34 not in ids[i['image_block_id']] for g in plan['groups'] for i in g['items']),
                '已知水印误检混入选项组')
    row = dict(page_id=spec['id'], structure_checks_passed=True, source_image_sha256=sha(image),
               raster_size=page['raster_size'], document_status=document['status'],
               block_statuses=dict(Counter(b['status'] for b in blocks.values())),
               region_types=dict(Counter(b['type'] for b in blocks.values())),
               actual_Ovis_regions=sum(b['type'] in ('text', 'formula', 'table') for b in blocks.values()),
               image_Ovis_attempts=sum(len(b['provenance']['recognition']['attempts'])
                                       for b in blocks.values() if b['type'] == 'image'),
               stop_reasons=dict(Counter(r['stop_reason'] for r in manifest['regions'])),
               recognition_failures=[dict(block_id=b['id'], type=b['type'], status=b['status'], error=b['error'])
                                     for b in blocks.values() if b['status'] not in ('ok', 'skipped')],
               selected_candidates=len(selected), regions=len(regions), ownership_unique=True,
               candidate_evidence_and_region_crops_match_captured_baseline=True,
               schedule_equals_pre_recognition_plan=True, first_export_equals_reexport=True,
               groups=plan['groups'], captions=labels, ambiguous_caption_ids=plan['ambiguous_caption_ids'],
               common_GT_order=orders, command_receipt_sha256=sha(folder/'command.json'),
               previous_candidate_comparison=previous_comparison,
               baseline_document_sha256=sha(args.before/spec['id']/'baseline/document.json'),
               reexport_command=command,
               artifact_sha256={str(p.relative_to(job)): sha(p) for p in sorted(job.rglob('*')) if p.is_file()})
    save(folder/'acceptance.json', row)
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', type=Path, required=True)
    parser.add_argument('--before', type=Path, required=True, help='配对重放保存的旧核心 baseline')
    parser.add_argument('--previous', type=Path, help='上一轮已通过的完整生产作业，额外核验无新增结构退化')
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--only', nargs='+', help='进行中的局部检查，不能作为20页最终验收')
    args = parser.parse_args()
    args.run = args.run.resolve()
    manifest_path = ROOT/'docs/omnidocbench-20/manifest.json'
    manifest, freeze = read(manifest_path), read(args.run/'freeze.json')
    require(sha(manifest_path) == freeze['manifest_sha256'], '冻结清单改变')
    require(sha(args.data/'OmniDocBench.json') == freeze['annotation_sha256'], '冻结标注改变')
    require(sha(args.run/'.runtime/config.json') == freeze['config_sha256'], '冻结配置改变')
    require(all(sha(ROOT/p) == h for p, h in freeze['source_sha256'].items()), '冻结源码改变')
    require(all(sha(args.run/p) == h for p, h in freeze['model_files'].items()), '冻结模型改变')
    require(all(sha(args.run/'.runtime'/name) == record['sha256'] for name, record in freeze['runtime'].items()),
            '冻结运行库改变')
    environment = dict(os.environ, LD_LIBRARY_PATH=str(args.run/'.runtime'))
    actual = runtime_sources(args.run/'.runtime/dococr_cli', environment)
    require(set(actual) == set(freeze['runtime']) and
            all(p == args.run/'.runtime'/name for name, p in actual.items()), '实际运行库未使用快照')
    if args.only:
        require(set(args.only) <= {s['id'] for s in manifest['pages']}, '未知页面')
    else:
        summary = read(args.run/'run-summary.json')
        require(summary['all_jobs_completed'] and summary['completed'] == 20, '完整20页未完成')
    report = dict(execution=freeze['execution'], candidate_freeze_sha256=sha(args.run/'freeze.json'),
                  verifier_sha256=sha(Path(__file__)), reference_report_sha256=sha(args.before/'summary.json'),
                  scope='focused_pages_not_full_20_or_OCR_quality' if args.only else
                        'narrowed_issue26_structure_acceptance_not_full_OCR_quality', pages=[], failures=[])
    for spec, annotation in zip(manifest['pages'], read(args.data/'OmniDocBench.json')):
        if args.only and spec['id'] not in args.only:
            continue
        try:
            row = verify_page(spec, annotation, args, freeze, environment)
            report['pages'].append(row)
            order = row['common_GT_order']
            print(f'{spec["id"]}: PASS {row["regions"]} regions; {len(row["groups"])} groups; '
                  f'{len(row["captions"])} captions; GT fixed={len(order["fixed"])} new={len(order["newly_wrong"])}',
                  flush=True)
        except (ValueError, jsonschema.ValidationError) as error:
            report['failures'].append(dict(page_id=spec['id'], reason=str(error)))
            print(f'{spec["id"]}: FAIL {error}', flush=True)
    report.update(complete_twenty_page_acceptance=not args.only and len(report['pages']) == 20 and
                  not report['failures'], inspected=len(report['pages'])+len(report['failures']))
    save(args.run/('acceptance-partial.json' if args.only else 'acceptance.json'), report)
    return 1 if report['failures'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
