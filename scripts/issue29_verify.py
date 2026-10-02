"""Issue #29：真实 Layout 前后对照，以及历史模型输出经公共完整流水线重放。"""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from PIL import Image
from layout_candidate_review import make_review

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tests'))
from printed_page_integration import config


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2)+'\n')


def run(cli, folder, image, setting, trace=None):
    folder.mkdir(parents=True)
    save(folder/'config.json', setting)
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = str(cli.parent) + os.pathsep + env.get("LD_LIBRARY_PATH", "")
    if trace is not None:
        save(folder/'fixture.json', trace)
        env['DOCOCR_TEST_STRUCTURE_PATH'] = str(folder/'fixture.json')
    command = [str(cli), '--config', str(folder/'config.json'), '--input', str(image), '--out', str(folder/'job')]
    result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True)
    save(folder/'command.json', dict(command=command, returncode=result.returncode, binary_sha256=sha(cli), LD_LIBRARY_PATH=env["LD_LIBRARY_PATH"]))
    (folder/'stderr.log').write_text(result.stderr)
    (folder/'stdout.log').write_text(result.stdout)
    if result.returncode:
        raise RuntimeError(result.stderr)
    return folder/'job', json.loads((folder/'job/document.json').read_text())


def content_by_candidate(job, doc):
    page = doc['pages'][0]
    layouts = {l['id']:l['candidate_id'] for l in page['layout_blocks']}
    regions = {r['id']:r for r in page['regions']}
    blocks = {}
    identities = {}
    for b in page['blocks']:
        region = regions[b['source_region_ids'][0]]
        ids = tuple(layouts[lid] for lid in region['source_layout_block_ids'])
        identities[b['id']] = ids
        blocks[ids] = dict(bbox=b['bbox'], type=b['type'], status=b['status'],
                          text=b['content']['text'], raw=b['provenance']['raw_output'],
                          crop_sha256=sha(job/b['content']['resource']))
    groups = [[(identities[i['image_block_id']], identities.get(i['caption_block_id'])) for i in g['items']]
              for g in page['structure_plan']['groups']]
    return blocks, [identities[b] for b in page['reading_order']], groups


def archive(folder, destination):
    destination.mkdir(parents=True)
    for name in ['document.json','document.md','run-manifest.json','execution-plan.json']:
        (destination/(name+'.gz')).write_bytes(gzip.compress((folder/name).read_bytes(), mtime=0))
    d=json.loads((folder/'document.json').read_text())
    diag=d['layout_diagnostics']
    paths=list(diag['raw_tensor_assets'].values())
    # Large input float tensor is hash-recorded in inventory; model outputs remain portable.
    paths=[p for p in paths if not p.endswith('image.f32')]
    if 'candidate_reviews' in diag:
        if diag['candidate_reviews']['source_asset']:
            paths.append(diag['candidate_reviews']['source_asset'])
        paths.extend(x['crop_asset'] for x in diag['candidate_reviews']['decisions'] if x['crop_asset'])
        paths.extend(c['mask_asset'] for c in diag['candidates'] if c['filter_reason'] == 'review_confirmed_watermark')
    for path in paths:
        target=destination/(Path(path).name+'.gz')
        target.write_bytes(gzip.compress((folder/path).read_bytes(),mtime=0))
    save(destination/'inventory.json', {str(p.relative_to(folder)):sha(p) for p in sorted(folder.rglob('*')) if p.is_file()})


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli',type=Path,required=True)
    parser.add_argument('--fixture-cli',type=Path,required=True)
    parser.add_argument('--baseline-cli',type=Path,required=True)
    parser.add_argument('--baseline-fixture-cli',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--archive',type=Path,required=True)
    args=parser.parse_args()
    args.out=args.out.resolve();args.archive=args.archive.resolve()
    args.out.mkdir(parents=True,exist_ok=False)
    manifest=json.loads((ROOT/'docs/omnidocbench-20/manifest.json').read_text())
    report=dict(base_commit=(ROOT/'output/issue29/base-commit.txt').read_text().strip(),
                sources={str(p.relative_to(ROOT)):sha(p) for p in sorted((ROOT/'src').glob('*')) if p.is_file()},
                binaries={str(p):sha(p) for p in [args.cli,args.fixture_cli,args.baseline_cli,args.baseline_fixture_cli]},
                libraries={str(p):sha(p) for folder in (args.cli.parent,args.baseline_cli.parent) for p in sorted(folder.glob('*.so'))},
                preexisting_patch_sha256=sha(ROOT/'output/issue29/preexisting.patch'),
                real={},replay={})
    watermark=None
    for pid in ['odb-03','odb-07','odb-08','odb-13']:
        spec=next(p for p in manifest['pages'] if p['id']==pid)
        image=ROOT/'output/omnidocbench/selected-20'/spec['image_path']
        assert sha(image)==spec['image_sha256']
        setting=json.load(gzip.open(ROOT/f'docs/issue-28/evidence/runs/{pid}/config.json.gz'))
        before,old=run(args.baseline_cli.resolve(),args.out/pid/'before',image,setting)
        setting['execution']['layout_candidate_reviews']=[]
        collect,doc=run(args.cli.resolve(),args.out/pid/'collect',image,setting)
        assert doc['pages']==old['pages'], pid
        if pid=='odb-03':
            watermark=make_review(collect,doc,34,'confirmed_watermark',
                '原页 c34 是重复的浅蓝精华在线背景标志片段；页面右侧和底部重复同形标志。此框不含坐标轴、曲线、题目或选项标签；页眉同源标志及八幅选项图保留。')
            save(args.archive/'odb-03-review.json',watermark)
        setting['execution']['layout_candidate_reviews']=[watermark]
        after,new=run(args.cli.resolve(),args.out/pid/'after',image,setting)
        a,ao,ag=content_by_candidate(before,old);b,bo,bg=content_by_candidate(after,new)
        if pid=='odb-03':
            del a[(34,)];ao.remove((34,))
            assert new['layout_diagnostics']['candidate_reviews']['decisions'][0]['outcome']=='skipped'
        else:
            assert new['layout_diagnostics']['candidate_reviews']['decisions'][0]['outcome']=='page_mismatch'
        assert a==b and ao==bo and ag==bg, pid
        subprocess.run([str(args.cli.resolve()),'--reexport',str(after/'document.json'),'--asset-root',str(after),'--out',str(args.out/pid/'reexport')],check=True,capture_output=True)
        assert (args.out/pid/'reexport/document.md').read_bytes()==(after/'document.md').read_bytes()
        archive(before,args.archive/pid/'before');archive(after,args.archive/pid/'after')
        # Directly viewable original and reviewed crop for diagnosis.
        if pid=='odb-03':
            shutil.copyfile(image,args.archive/'odb-03-original.jpg')
            r=new['layout_diagnostics']['candidate_reviews']
            shutil.copyfile(after/r['decisions'][0]['crop_asset'],args.archive/'odb-03-c34.png')
            shutil.copyfile(after/new['layout_diagnostics']['candidates'][34]['mask_asset'],args.archive/'odb-03-c34-mask.png')
        report['real'][pid]=dict(input_sha256=sha(image),before=str(before),after=str(after),
            unchanged_candidates=len(a),removed=[34] if pid=='odb-03' else [],
            checks=['content_and_crop_bytes','reading_order','option_groups','reexport'])
        save(args.archive/'verification.json',report)
        print(pid,'real Layout PASS',flush=True)
    # Replay ALL 20 real pages using frozen Layout tensors/masks and captured Ovis outputs.
    # This isolates structural effects; it is explicitly not a new Ovis quality measurement.
    for spec in manifest['pages']:
        pid=spec['id'];image=ROOT/'output/omnidocbench/selected-20'/spec['image_path']
        source=ROOT/'output/issue-26/acceptance-20-v1.9'/pid/'job'
        captured=json.loads((source/'document.json').read_text())
        diag=captured['layout_diagnostics'];tensors=diag['raw_tensor_assets']
        trace=dict(candidate_tensor_path=str(source/tensors['fetch_name_0']),candidate_count=diag['candidate_count'],
                   mask_rle_path=str(source/tensors['fetch_name_2']),outputs=[])
        for block in captured['pages'][0]['blocks']:
            if block['type'] in ('text','formula','table'):
                trace['outputs'].append(dict(bbox=block['bbox'],text=block['provenance']['raw_output'] or ''))
        setting=config('printed_page_structure')
        setting['execution'].update(layout_preprocess='smartresize_lanczos',layout_score_threshold=.3,max_page_pixels=64000000,max_output_bytes=67108864)
        before,old=run(args.baseline_fixture_cli.resolve(),args.out/'replay'/pid/'before',image,setting,trace)
        setting['execution']['layout_candidate_reviews']=[watermark]
        after,new=run(args.fixture_cli.resolve(),args.out/'replay'/pid/'after',image,setting,trace)
        a,ao,ag=content_by_candidate(before,old);b,bo,bg=content_by_candidate(after,new)
        if pid=='odb-03':
            assert new['layout_diagnostics']['candidate_reviews']['decisions'][0]['outcome']=='skipped'
            del a[(34,)];ao.remove((34,))
            assert len(bg)==2 and all(len(g)==4 for g in bg)
        assert a==b and ao==bo and ag==bg,pid
        # Every retained source belongs to exactly one Region: no new omissions/duplicates/mixed ownership.
        def ownership(d):
            p=d['pages'][0];ls={l['id']:l['candidate_id'] for l in p['layout_blocks']}
            ids=[ls[x] for r in p['regions'] for x in r['source_layout_block_ids']]
            assert len(ids)==len(set(ids)) and set(ids)==set(ls.values())
        ownership(new)
        report['replay'][pid]=dict(execution='captured_model_outputs_public_fixture_job',source_document_sha256=sha(source/'document.json'),
            source_tensor_sha256=sha(source/tensors['fetch_name_0']),source_masks_sha256=sha(source/tensors['fetch_name_2']),
            before_document_sha256=sha(before/'document.json'),after_document_sha256=sha(after/'document.json'),
            unchanged_regions=len(a),removed=[34] if pid=='odb-03' else [],
            checks=['content_status_raw_output','crop_pixels','reading_order','option_groups','unique_ownership'])
        if pid in ('odb-03','odb-07','odb-08'):
            archive(before,args.archive/'replay'/pid/'before');archive(after,args.archive/'replay'/pid/'after')
        save(args.archive/'verification.json',report)
        print(pid,'captured full pipeline PASS',flush=True)


if __name__=='__main__':
    main()
