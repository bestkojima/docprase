"""公共 CLI 重放冻结模型张量与历史转写，单独核验结构修改，不报告新模型精度。"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

from issue17_baseline import score_page

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return json.loads(path.read_text())


def sha(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')


def order_comparison(annotation, before, after):
    scores = score_page(annotation, before)
    comparable = []
    for category in ('text', 'independent_formula', 'table', 'figure'):
        for row in scores[category]['rows']:
            if row['block_id'] and isinstance(row['order'], int):
                comparable.append((row['order'], row['annotation_id'], row['block_id']))
    comparable.sort(key=lambda row: row[0])
    old = {bid: i for i, bid in enumerate(before['pages'][0]['reading_order'])}
    new = {bid: i for i, bid in enumerate(after['pages'][0]['reading_order'])}
    fixed, added, total = [], [], 0
    for i, a in enumerate(comparable):
        for b in comparable[i + 1:]:
            if a[2] == b[2] or a[0] == b[0]:
                continue
            total += 1
            was = old[a[2]] < old[b[2]]
            now = new[a[2]] < new[b[2]]
            row = dict(annotation_ids=[a[1], b[1]], block_ids=[a[2], b[2]])
            if not was and now:
                fixed.append(row)
            elif was and not now:
                added.append(row)
    return dict(denominator=total, fixed=fixed, newly_wrong=added,
                basis='fixed_baseline_GT_pairing; image-caption semantic adjacency may differ from GT row order')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture-cli', type=Path, required=True)
    parser.add_argument('--production-cli', type=Path, required=True)
    parser.add_argument('--baseline-cli', type=Path, required=True)
    parser.add_argument('--baseline-library-root', type=Path, required=True)
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--only', nargs='+')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    manifest = read(ROOT / 'docs/omnidocbench-20/manifest.json')
    annotation_path = args.data / 'OmniDocBench.json'
    if sha(annotation_path) != manifest['subset_annotation_sha256']:
        raise ValueError('冻结标注哈希不符')
    annotations = read(annotation_path)
    if len(annotations) != len(manifest['pages']):
        raise ValueError('冻结标注页数不符')
    if args.only and set(args.only) - {p['id'] for p in manifest['pages']}:
        parser.error('--only 包含未知页面')
    report = dict(execution='captured_model_tensors_and_transcription_replay_not_fresh_model_inference',
                  fixture_cli_sha256=sha(args.fixture_cli), production_cli_sha256=sha(args.production_cli),
                  fixture_library_sha256=sha(args.fixture_cli.parent / 'libdococr_c_test.so'),
                  baseline_library_sha256=sha(args.baseline_library_root / 'libdococr_c_test.so'),
                  baseline_core_source_sha256=sha(args.baseline_library_root / 'core.cpp'),
                  fixture_backend_source_sha256=sha(ROOT / 'tests/fixture_backend.cpp'),
                  annotation_sha256=manifest['subset_annotation_sha256'],
                  core_source_sha256=sha(ROOT / 'src/core.cpp'),
                  structure_source_sha256=sha(ROOT / 'src/region_structure.cpp'), pages=[])
    for spec, annotation in zip(manifest['pages'], annotations):
        if args.only and spec['id'] not in args.only:
            continue
        source = args.before / spec['id'] / 'job'
        before = read(source / 'document.json')
        original_sha = sha(source / 'document.json')
        image = args.data / spec['image_path']
        if sha(image) != spec['image_sha256']:
            raise ValueError('冻结原图哈希不符')
        folder = args.out / spec['id']
        folder.mkdir(exist_ok=True)
        original_config = read(source / 'execution-plan.json')['effective_config']
        cfg = read(ROOT / 'configs/printed-page.example.json')
        cfg.update(mode='development', backend='fixture:printed_page_structure')
        cfg['models'] = copy.deepcopy(read(ROOT / 'configs/fixture-plan.example.json')['models'])
        cfg['execution'] = original_config['execution']
        cfg['platform'] = original_config['platform']
        save(folder / 'config.json', cfg)
        diag = before['layout_diagnostics']
        raw_path = source / diag['raw_tensor_assets']['fetch_name_0']
        mask_path = source / diag['raw_tensor_assets']['fetch_name_2']
        fixture = dict(candidate_tensor_path=str(raw_path.resolve()), candidate_count=diag['candidate_count'],
                       mask_rle_path=str(mask_path.resolve()), outputs=[])
        for block in before['pages'][0]['blocks']:
            if block['type'] not in ('text', 'formula', 'table'):
                continue
            provenance = block['provenance']
            attempts = provenance.get('recognition', {}).get('attempts', [])
            if attempts:
                output = copy.deepcopy(attempts[-1]['output'])
            else:
                # 1.5 历史输出只有 raw_output 与视觉 stop_reason，无法补造完整尝试。
                # 两侧都用同一原文和停止状态重放，以隔离当前校验策略的差异。
                stop = provenance['visual']['stop_reason']
                output = dict(text=provenance['raw_output'], raw_output=provenance['raw_output'],
                              stop_reason=stop, finish_reason='complete' if stop == 'normal' else
                              'truncated' if stop == 'token_limit' else 'failed',
                              error='' if stop in ('normal', 'token_limit') else block['error'] or '')
            if output['text'] is None or output['raw_output'] is None:
                raise ValueError('此重放不支持历史非法 UTF-8')
            output['error'] = output['error'] or ''
            output['bbox'] = block['bbox']
            fixture['outputs'].append(output)
        save(folder / 'fixture.json', fixture)
        baseline = folder / 'baseline'
        old_command = [str(args.baseline_cli.resolve()), '--config', str((folder/'config.json').resolve()),
                       '--input', str(image.resolve()), '--out', str(baseline.resolve())]
        old_env = dict(os.environ, DOCOCR_TEST_STRUCTURE_PATH=str((folder/'fixture.json').resolve()),
                       LD_LIBRARY_PATH=str(args.baseline_library_root.resolve()))
        old_result = subprocess.run(old_command, cwd=ROOT, env=old_env, capture_output=True, text=True)
        (folder / 'baseline.log').write_text(old_result.stdout + old_result.stderr)
        if old_result.returncode:
            raise RuntimeError(old_result.stderr)
        baseline_document = read(baseline / 'document.json')
        command = [str(args.fixture_cli.resolve()), '--config', str((folder / 'config.json').resolve()),
                   '--input', str(image.resolve()), '--out', str((folder / 'job').resolve())]
        start = time.monotonic()
        result = subprocess.run(command, cwd=ROOT,
            env=dict(os.environ, DOCOCR_TEST_STRUCTURE_PATH=str((folder / 'fixture.json').resolve())),
            capture_output=True, text=True)
        (folder / 'run.log').write_text(result.stdout + result.stderr)
        receipt = dict(command=command, returncode=result.returncode,
            input_sha256=spec['image_sha256'], elapsed_seconds=time.monotonic()-start,
            execution=report['execution'], historical_document_sha256=original_sha,
            baseline_command=old_command, baseline_returncode=old_result.returncode,
            source_tensor_sha256=sha(raw_path), source_mask_sha256=sha(mask_path),
            provenance=dict(fixture_cli_sha256=report['fixture_cli_sha256'],
                            fixture_library_sha256=report['fixture_library_sha256'],
                            baseline_library_sha256=report['baseline_library_sha256'],
                            source_runtime=read(source.parent / 'command.json')['provenance']))
        save(folder / 'command.json', receipt)
        if result.returncode:
            raise RuntimeError(f'{spec["id"]} 公共重放失败：{result.stderr}')
        after = read(folder / 'job/document.json')
        if sha(source / 'document.json') != original_sha:
            raise ValueError('重放期间历史输出变化')
        previous = before['pages'][0]
        current = after['pages'][0]
        before_candidates = {c['candidate_id'] for c in diag['candidates'] if c['selected']}
        after_candidates = {c['candidate_id'] for c in after['layout_diagnostics']['candidates'] if c['selected']}
        if before_candidates != after_candidates:
            raise ValueError(f'{spec["id"]}: 重放候选集合变化')
        if diag['candidates'] != after['layout_diagnostics']['candidates']:
            raise ValueError(f'{spec["id"]}: 原始候选、rank、过滤理由或 mask 引用变化')
        if sha(raw_path) != sha(folder/'job'/after['layout_diagnostics']['raw_tensor_assets']['fetch_name_0']) or \
           sha(mask_path) != sha(folder/'job'/after['layout_diagnostics']['raw_tensor_assets']['fetch_name_2']):
            raise ValueError(f'{spec["id"]}: 检测张量或 mask 字节变化')
        old_regions = {r['id']: (r['bbox'], r['source_layout_block_ids']) for r in previous['regions']}
        new_regions = {r['id']: (r['bbox'], r['source_layout_block_ids']) for r in current['regions']}
        if old_regions != new_regions:
            raise ValueError(f'{spec["id"]}: Region 身份、裁图或归属变化')
        old_blocks = {b['id']: b for b in previous['blocks']}
        for block in current['blocks']:
            resource = block['content']['resource']
            if resource != old_blocks[block['id']]['content']['resource'] or sha(source/resource) != sha(folder/'job'/resource):
                raise ValueError(f'{spec["id"]}: 原图裁图资源变化')
        old_text = {b['id']: (b['content'], b['status'], b['error']) for b in baseline_document['pages'][0]['blocks']}
        new_text = {b['id']: (b['content'], b['status'], b['error']) for b in current['blocks']}
        if old_text != new_text:
            raise ValueError(f'{spec["id"]}: 历史转写重放结果变化')
        result = subprocess.run([str(args.production_cli.resolve()), '--reexport', str((folder/'job/document.json').resolve()),
            '--asset-root', str((folder/'job').resolve()), '--out', str((folder/'reexport').resolve())],
            cwd=ROOT, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr)
        if (folder/'job/document.md').read_bytes() != (folder/'reexport/document.md').read_bytes():
            raise ValueError('首次导出与离线导出不同')
        row = dict(page_id=spec['id'], source_image_sha256=spec['image_sha256'],
            before_document_sha256=original_sha, after_document_sha256=sha(folder/'job/document.json'),
            selected_candidates=len(after_candidates), regions=len(new_regions),
            unchanged_candidates_regions_ownership_and_transcription=True,
            first_export_equals_reexport=True, groups=current['structure_plan']['groups'],
            captions=current['structure_plan']['captions'],
            ambiguous_caption_ids=current['structure_plan']['ambiguous_caption_ids'],
            reading_order=order_comparison(annotation, baseline_document, after))
        report['pages'].append(row)
        save(args.out / 'summary.json', report)
        print(f'{spec["id"]}: PASS {len(new_regions)} regions, {len(row["groups"])} image rows, '
              f'{len(row["captions"])} captions; GT fixed={len(row["reading_order"]["fixed"])} '
              f'new={len(row["reading_order"]["newly_wrong"])}', flush=True)
    print(f'PASS: {len(report["pages"])} complete pages replayed through public CLI', flush=True)


if __name__ == '__main__':
    main()
