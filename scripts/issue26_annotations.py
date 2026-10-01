"""从真实公共作业重放 DocLayout 标注 PNG；不执行推理或修改后处理。"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import shutil
import sys
import zipfile

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[1]
COLORS = {'text': '#1262ac', 'image': '#c73524', 'formula': '#9327a1',
          'table': '#08744c', 'owned': '#777777'}
FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'
HEADER = 96
STAGES = {
    '01-score-filtered': ('Score-filtered original boxes', 'Before local NMS / containment / overlap filtering'),
    '02-postprocessed': ('Postprocessed layout boxes', 'Selected crop_bbox; owned children shown in gray'),
    '03-regions': ('Saved recognition regions', 'Final saved Region bounds; dashed outline = expanded owner'),
    '04-reading-order': ('Saved final reading order', '# = final order; dashed vertical line = page midpoint'),
    '05-ground-truth': ('Upstream ground truth', 'Reference annotations; not model detections'),
}


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def read(path):
    return json.loads(path.read_text())


def write(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')


def kind(label):
    if label in ('formula', 'equation_isolated', 'equation_inline'):
        return 'formula'
    if label in ('image', 'chart', 'seal', 'figure'):
        return 'image'
    return 'table' if label == 'table' else 'text'


def block_candidates(page):
    layouts = {b['id']: b for b in page['layout_blocks']}
    regions = {r['id']: r for r in page['regions']}
    return {b['id']: [layouts[lid]['candidate_id']
                     for rid in b['source_region_ids']
                     for lid in regions[rid]['source_layout_block_ids']]
            for b in page['blocks']}


def check_options(document):
    """历史真实公共作业的重放检查：两题各四个图片须从左到右。"""
    page = document['pages'][0]
    ids = block_candidates(page)
    layouts = {b['candidate_id']: b for b in page['layout_blocks']}
    passed = True
    # 此固定原页的第17/18题已确认图片候选，不纳入水印误检。
    for question, candidates in [(17, {8, 5, 6, 13}), (18, {4, 1, 0, 2})]:
        missing = candidates - layouts.keys()
        expected = sorted(candidates, key=lambda cid: layouts[cid]['bbox'][0]) if not missing else []
        actual = [cid for bid in page['reading_order'] for cid in ids[bid] if cid in candidates]
        ok = not missing and actual == expected
        passed &= ok
        print(f'Q{question}: {"PASS" if ok else "FAIL"} expected={expected} actual={actual}', flush=True)
    return passed


def entries(document, annotation, stage):
    diagnostic = document['layout_diagnostics']
    candidates = {c['candidate_id']: c for c in diagnostic['candidates']}
    page = document['pages'][0]
    ids = block_candidates(page)
    order = {bid: i for i, bid in enumerate(page['reading_order'], 1)}
    owners = {r['source_layout_block_id']: r['owner_block_id']
              for r in page['relations'] if r['type'] == 'content_owned_by'}
    owned_candidates = {b['candidate_id'] for b in page['layout_blocks'] if b['id'] in owners}
    if stage in ('01-score-filtered', '02-postprocessed'):
        for c in candidates.values():
            if stage == '01-score-filtered' and c['score'] < diagnostic['score_threshold']:
                continue
            if stage == '02-postprocessed' and not c['selected']:
                continue
            bbox = c['original_bbox'] if stage == '01-score-filtered' else c['crop_bbox']
            owned = stage == '02-postprocessed' and c['candidate_id'] in owned_candidates
            label = f'c{c["candidate_id"]} {c["model_label"]} {c["score"]:.2f} k{c["rank"]}'
            if owned:
                label += ' owned'
            yield bbox, label, 'owned' if owned else kind(c['model_label']), owned
    elif stage in ('03-regions', '04-reading-order'):
        regions = {r['id']: r for r in page['regions']}
        for b in page['blocks']:
            region = regions[b['source_region_ids'][0]]
            c = candidates[ids[b['id']][0]]
            label = (f'#{order[b["id"]]:02d}' if stage == '04-reading-order' else region['id'])
            label += f' c{c["candidate_id"]} {c["model_label"]}'
            expanded = c['recognition_crop_bbox'] is not None
            yield region['bbox'], label, kind(c['model_label']), expanded
    else:
        for a in annotation['layout_dets']:
            if a.get('ignore'):
                continue
            poly = a['poly']
            bbox = [min(poly[::2]), min(poly[1::2]), max(poly[::2]), max(poly[1::2])]
            yield bbox, f'GT{a["anno_id"]} {a["category_type"]}', kind(a['category_type']), False


def render(source, document, annotation, stage, page_id, max_edge, viewport=None, source_label='Saved public job'):
    viewport = viewport or (0, 0, source.width, source.height)
    base = source.crop(viewport)
    scale = min(1., max_edge / max(base.size)) if max_edge else 1.
    base = base.resize((round(base.width * scale), round(base.height * scale)), Image.Resampling.LANCZOS)
    canvas = Image.new('RGB', (base.width, base.height + HEADER), 'white')
    canvas.paste(base, (0, HEADER))
    draw = ImageDraw.Draw(canvas)
    font = ImageFont.truetype(FONT, 15)
    heading = ImageFont.truetype(FONT, 21)
    draw.text((12, 8), f'{page_id} | {STAGES[stage][0]}', font=heading, fill='#17212b')
    draw.text((12, 36), STAGES[stage][1], font=font, fill='#3e4b57')
    draw.text((12, 58), 'Blue text | Red image | Purple formula | Green table | Gray owned', font=font, fill='#3e4b57')
    draw.text((12, 77), f'{source_label}; Lanczos / 0.3; render scale={scale:.4f}', font=font, fill='#3e4b57')
    if stage == '04-reading-order':
        mid = (source.width / 2 - viewport[0]) * scale
        for y in range(HEADER, canvas.height, 16):
            draw.line((mid, y, mid, min(y + 8, canvas.height)), fill='#aaaaaa', width=1)
    for bbox, label, category, dashed in entries(document, annotation, stage):
        x0, y0, x1, y1 = bbox
        if x1 <= viewport[0] or x0 >= viewport[2] or y1 <= viewport[1] or y0 >= viewport[3]:
            continue
        x0, x1 = [min(base.width - 1, max(0, round((x - viewport[0]) * scale))) for x in (x0, x1)]
        y0, y1 = [min(canvas.height - 1, max(HEADER, round((y - viewport[1]) * scale) + HEADER)) for y in (y0, y1)]
        if x1 <= x0 or y1 <= y0:
            continue
        color = COLORS[category]
        if dashed:
            for x in range(x0, x1, 14):
                for y in (y0, y1):
                    draw.line((x, y, min(x + 7, x1), y), fill=color, width=2)
            for y in range(y0, y1, 14):
                for x in (x0, x1):
                    draw.line((x, y, x, min(y + 7, y1)), fill=color, width=2)
        else:
            draw.rectangle((x0, y0, x1, y1), outline=color, width=2)
        # 标签放在框上方；图中保留完整框，详细坐标另存 JSON。
        tw = round(draw.textlength(label, font=font)) + 8
        tx = max(0, min(x0, base.width - tw))
        ty = max(HEADER, y0 - 20)
        draw.rectangle((tx, ty, tx + tw, ty + 19), fill='white')
        draw.text((tx + 3, ty), label, font=font, fill=color)
    return canvas, scale


def export_page(spec, annotation, args):
    job = args.jobs / spec['id'] / 'job'
    image_path = args.data / spec['image_path']
    receipt = read(job.parent / 'command.json')
    if sha(image_path) != spec['image_sha256'] or receipt['input_sha256'] != spec['image_sha256']:
        raise ValueError(f'{spec["id"]}: 原图与公共作业输入哈希不符')
    if receipt['returncode'] != 0:
        raise ValueError(f'{spec["id"]}: 原公共作业未成功退出')
    document = read(job / 'document.json')
    plan = read(job / 'execution-plan.json')
    manifest = read(job / 'run-manifest.json')
    if manifest['config_hash'] != plan['config_hash']:
        raise ValueError(f'{spec["id"]}: 运行清单与配置不符')
    diagnostic = document['layout_diagnostics']
    execution = plan['effective_config']['execution']
    if diagnostic['score_threshold'] != execution['layout_score_threshold']:
        raise ValueError(f'{spec["id"]}: 分数阈值不符')
    # 校验图片来自完整原页，且选中候选确实映射到保存的版面块。
    selected = {c['candidate_id'] for c in diagnostic['candidates'] if c['selected']}
    if selected != {b['candidate_id'] for b in document['pages'][0]['layout_blocks']}:
        raise ValueError(f'{spec["id"]}: 候选与版面块不符')
    target = args.out / spec['id']
    target.mkdir(parents=True, exist_ok=True)
    for name in ('document.json', 'run-manifest.json', 'execution-plan.json'):
        shutil.copyfile(job / name, target / name)
    shutil.copyfile(job.parent / 'command.json', target / 'command.json')
    write(target / 'ground-truth.json', annotation)
    write(target / 'candidates.json', diagnostic['candidates'])
    record = dict(page_id=spec['id'], source_image=str(image_path.resolve()),
                  source_image_sha256=spec['image_sha256'], source_job=str(job.resolve()),
                  source_document_sha256=sha(job / 'document.json'),
                  execution='rendered_saved_public_job_not_new_inference',
                  source_execution=receipt.get('execution', 'historical_public_model_inference'),
                  source_runtime=receipt['provenance'], config_hash=manifest['config_hash'],
                  preprocessing=execution['layout_preprocess'], score_threshold=diagnostic['score_threshold'],
                  raw_candidates=len(diagnostic['candidates']), selected_candidates=len(selected),
                  filter_reasons=dict(Counter(c['filter_reason'] for c in diagnostic['candidates'] if not c['selected'])),
                  source_size=[spec['width'], spec['height']], pngs={})
    with Image.open(image_path) as loaded:
        source = loaded.convert('RGB')
    if source.size != (spec['width'], spec['height']):
        raise ValueError(f'{spec["id"]}: 原图尺寸不符')
    for stage in STAGES:
        canvas, scale = render(source, document, annotation, stage, spec['id'], args.max_edge,
                               source_label=args.source_label)
        name = stage + '.png'
        canvas.save(target / name)
        record['pngs'][name] = dict(sha256=sha(target / name), size=list(canvas.size), scale=scale,
                                   page_origin_in_png=[0, HEADER])
    if spec['id'] == 'odb-03':
        for question, viewport in [(17, (40, 320, 1370, 745)), (18, (100, 705, 1220, 1120))]:
            panels = [render(source, document, annotation, stage, f'odb-03 Q{question}', 0, viewport,
                              source_label=args.source_label)[0]
                      for stage in ('01-score-filtered', '02-postprocessed', '04-reading-order')]
            comparison = Image.new('RGB', (panels[0].width, sum(p.height for p in panels) + 24), '#dce2e8')
            y = 0
            for panel in panels:
                comparison.paste(panel, (0, y))
                y += panel.height + 12
            name = f'q{question}-comparison.png'
            comparison.save(target / name)
            record['pngs'][name] = dict(sha256=sha(target / name), size=list(comparison.size), source_viewport=viewport)
    source.close()
    write(target / 'evidence.json', record)
    print(f'{spec["id"]}: {record["raw_candidates"]} raw -> {len(selected)} selected; {len(record["pngs"])} PNG', flush=True)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jobs', type=Path, required=True)
    parser.add_argument('--data', type=Path)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--only', nargs='+')
    parser.add_argument('--max-edge', type=int, default=4096, help='0 为原图尺寸，默认只缩小超大原页')
    parser.add_argument('--source-label', default='Saved public job', help='PNG 标头的作业来源说明')
    parser.add_argument('--check-order', action='store_true', help='重放 odb-03 选项图片顺序，错误返回1')
    args = parser.parse_args()
    if args.check_order:
        return 0 if check_options(read(args.jobs / 'odb-03/job/document.json')) else 1
    if args.data is None or args.out is None:
        parser.error('导出需要 --data 和 --out')
    if args.max_edge < 0:
        parser.error('--max-edge 不能小于0')
    manifest = read(ROOT / 'docs/omnidocbench-20/manifest.json')
    annotation_path = args.data / 'OmniDocBench.json'
    if sha(annotation_path) != manifest['subset_annotation_sha256']:
        raise ValueError('标注文件哈希不符')
    annotations = read(annotation_path)
    if len(annotations) != len(manifest['pages']):
        raise ValueError('标注页数不符')
    if args.only and set(args.only) - {s['id'] for s in manifest['pages']}:
        parser.error('--only 包含未知页面')
    args.out.mkdir(parents=True, exist_ok=True)
    records = [export_page(s, a, args) for s, a in zip(manifest['pages'], annotations)
               if not args.only or s['id'] in args.only]
    write(args.out / 'manifest.json', dict(role='issue26_debug_intermediates', pages=records,
                                          stages=STAGES, max_edge=args.max_edge,
                                          renderer_sha256=sha(Path(__file__))))
    with zipfile.ZipFile(args.out / 'annotated-pngs.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
        for record in records:
            folder = args.out / record['page_id']
            for path in sorted(folder.iterdir()):
                archive.write(path, path.relative_to(args.out))
        archive.write(args.out / 'manifest.json', 'manifest.json')
    print(f'Exported {len(records)} pages; {sum(len(r["pngs"]) for r in records)} PNG; all source hashes checked.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
