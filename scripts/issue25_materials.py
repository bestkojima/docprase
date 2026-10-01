"""核对带标注材料、冻结评分文件与 #24 候选身份。"""
import hashlib
from contextlib import contextmanager
import json
import os
from pathlib import Path
import shutil
import tempfile
import zipfile

from PIL import Image

from issue17_baseline import ROOT
from issue20_runtime import runtime_sources
from issue24_run import candidate_intact, save, sha

MANIFEST_PATH = 'docs/issue-23/manifest.json'
THRESHOLDS_PATH = 'docs/issue-24/thresholds.json'
SCORERS = ('issue24_report.py', 'issue24_run.py', 'issue17_baseline.py',
           'issue15_quality.py', 'issue15_lineage.py', 'issue20_runtime.py')


def read_materials(manifest_path, annotations_path):
    manifest = json.loads(manifest_path.read_text())
    if sha(annotations_path) != manifest['subset_annotation']['sha256']:
        raise ValueError('参考标注子集 SHA 不符')
    annotations = json.loads(annotations_path.read_text())
    specs = manifest['pages']
    ids = [p['id'] for p in specs]
    if (len(annotations) != len(specs) or len(set(ids)) != len(ids) or
            ids != manifest['qualified_evaluation_ids'] or len(ids) != manifest['coverage']['page_count']):
        raise ValueError('封存页序、页数或资格清单不一致')
    for spec, annotation in zip(specs, annotations):
        canonical = json.dumps(annotation, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode()
        if hashlib.sha256(canonical).hexdigest() != spec['annotation_sha256']:
            raise ValueError(f"逐页参考 SHA 不符：{spec['id']}")
        if Path(spec['image_path']).name != annotation['page_info']['image_path']:
            raise ValueError(f"参考与原图不对应：{spec['id']}")
    return manifest, annotations


def material_files(manifest):
    files = {MANIFEST_PATH: sha(ROOT / MANIFEST_PATH),
             manifest['subset_annotation']['path']: manifest['subset_annotation']['sha256']}
    readme = manifest['upstream_readme']
    files[readme['local_path']] = readme['sha256']
    for spec in manifest['pages']:
        source = ROOT / spec['local_path']
        if sha(source) != spec['image_sha256']:
            raise ValueError(f"原图 SHA 不符：{spec['id']}")
        with Image.open(source) as image:
            if list(image.size) != spec['dimensions']:
                raise ValueError(f"原图尺寸不符：{spec['id']}")
        files[spec['local_path']] = spec['image_sha256']
    return files


def frozen_runtime(candidate_run, candidate):
    if not candidate_intact(candidate_run, candidate):
        raise ValueError('#24 候选模型、运行时、配置或门槛 SHA 不符')
    binary = candidate_run / 'candidate/runtime/dococr_cli'
    directory = binary.parent
    environment = dict(os.environ, LD_LIBRARY_PATH=str(directory))
    loaded = runtime_sources(binary, environment)
    if set(loaded) != set(candidate['runtime']) or any(
            path != (directory / name).resolve() for name, path in loaded.items()):
        raise ValueError('实际加载的项目动态库不属于冻结快照')
    return binary, candidate_run / 'candidate/config.json', environment


def freeze(output, candidate_run):
    manifest = json.loads((ROOT / MANIFEST_PATH).read_text())
    read_materials(ROOT / MANIFEST_PATH, ROOT / manifest['subset_annotation']['path'])
    files = material_files(manifest)
    candidate_path = candidate_run / 'candidate.json'
    candidate = json.loads(candidate_path.read_text())
    if sha(candidate_path) != sha(ROOT / 'docs/issue-24/evidence/candidate.json'):
        raise ValueError('候选身份与 #24 已提交记录不同')
    if sha(ROOT / THRESHOLDS_PATH) != candidate['thresholds_sha256']:
        raise ValueError('门槛不属于 #24 冻结版本')
    frozen_runtime(candidate_run, candidate)
    if any(sha(ROOT / name) != digest for name, digest in candidate['source_sha256'].items()):
        raise ValueError('当前生产源码与冻结候选不同')
    names = [THRESHOLDS_PATH, 'docs/issue-23/evidence/requirements.json',
             'docs/issue-23/materials.sha256', 'docs/issue-23/seal.sha256',
             'docs/issue-24/evidence/candidate.json', 'docs/issue-24/report.json',
             'docs/issue-24/engineering-conditions.json',
             'scripts/issue25_materials.py', 'scripts/issue25_run.py', 'scripts/issue25_report.py',
             'tests/issue25_real.py']
    names.extend(f'scripts/{name}' for name in SCORERS)
    conditions = json.loads((ROOT / 'docs/issue-24/engineering-conditions.json').read_text())
    for condition in conditions['conditions']:
        for name, digest in condition.get('evidence', {}).items():
            if sha(ROOT / name) != digest:
                raise ValueError(f'工程证据 SHA 不符：{name}')
            names.append(name)
    for name in names:
        files[name] = sha(ROOT / name)
    # 图像仍按封存 SHA 引用；其余元数据及评分实现保存独立副本。
    snapshots = {}
    image_names = {p['local_path'] for p in manifest['pages']}
    for name, digest in files.items():
        if name in image_names:
            continue
        target = output / 'freeze' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / name, target)
        if sha(target) != digest:
            raise ValueError(f'冻结复制 SHA 不符：{name}')
        snapshots[name] = digest
    record = dict(candidate_run=str(candidate_run), candidate_sha256=sha(candidate_path),
        material_freeze_id=manifest['freeze_id'], revision=manifest['revision'],
        source_files=files, snapshot_files=snapshots,
        execution='fresh_public_cli_jobs', strict_independent_holdout_claim=False,
        randomness='no_seed_configured_single_observation_not_bitwise_determinism')
    save(output / 'evaluation.json', record)
    return record, candidate, manifest


def verify_sources(record, candidate):
    return (all(sha(ROOT / name) == digest for name, digest in record['source_files'].items()) and
            sha(Path(record['candidate_run']) / 'candidate.json') == record['candidate_sha256'] and
            candidate_intact(Path(record['candidate_run']), candidate))


def read_frozen(run, entrypoint):
    record = json.loads((run / 'evaluation.json').read_text())
    for name, digest in record['snapshot_files'].items():
        if sha(run / 'freeze' / name) != digest:
            raise ValueError(f'评测冻结副本 SHA 不符：{name}')
    # 同时校验本次门槛判定、材料校验和验证器，不能只冻结旧评分函数。
    tools = {name: ROOT / name for name in record['snapshot_files']
             if name.startswith(('scripts/', 'tests/'))}
    tools['scripts/issue25_materials.py'] = Path(__file__)
    entrypoints = {'issue25_report.py': 'scripts/issue25_report.py',
                  'issue25_real.py': 'tests/issue25_real.py'}
    tools[entrypoints[entrypoint.name]] = entrypoint
    for name, path in tools.items():
        if sha(path) != record['snapshot_files'][name]:
            raise ValueError(f'当前评分工具与运行前冻结版本不同：{name}')
    manifest_path = run / 'freeze' / MANIFEST_PATH
    manifest = json.loads(manifest_path.read_text())
    manifest, annotations = read_materials(manifest_path,
        run / 'freeze' / manifest['subset_annotation']['path'])
    return record, manifest, annotations


@contextmanager
def restore_page(run, page_id):
    """从已核对每个原始文件 SHA 的无损包恢复一页；同时核对 Git 保留的副本。"""
    metadata = run / 'pages' / page_id
    index = json.loads((metadata / 'archive.json').read_text())
    archive = run / index['path']
    if sha(archive) != index['sha256']:
        raise ValueError(f'原始产物压缩包 SHA 不符：{page_id}')
    with tempfile.TemporaryDirectory(prefix='issue25-restore-') as temporary:
        folder = Path(temporary)
        with zipfile.ZipFile(archive) as bundle:
            if (len(bundle.namelist()) != len(index['files_sha256']) or
                    set(bundle.namelist()) != set(index['files_sha256'])):
                raise ValueError(f'压缩包文件清单不完整：{page_id}')
            for name, digest in index['files_sha256'].items():
                target = (folder / name).resolve()
                if not target.is_relative_to(folder) or name.startswith('/'):
                    raise ValueError(f'压缩包路径越界：{name}')
                contents = bundle.read(name)
                if hashlib.sha256(contents).hexdigest() != digest:
                    raise ValueError(f'原始产物文件 SHA 不符：{page_id}/{name}')
                retained = metadata / name
                if retained.exists() and sha(retained) != digest:
                    raise ValueError(f'已保留原始产物副本 SHA 不符：{page_id}/{name}')
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(contents)
        yield folder
