"""#27 的材料、工具版本和真实运行时证据边界。"""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

from issue17_baseline import DATA, MANIFEST, ROOT, verify_data
from issue24_run import candidate_intact, sha, save
from issue25_materials import MANIFEST_PATH, THRESHOLDS_PATH, material_files, read_materials, frozen_runtime

TOOLS = ('scripts/issue27_evidence.py', 'scripts/issue27_alignment.py', 'scripts/issue27_run.py',
         'scripts/issue27_report.py', 'tests/issue27_real.py', 'tests/issue22_real.py',
         'scripts/issue24_run.py', 'scripts/issue24_report.py', 'scripts/issue17_baseline.py',
         'scripts/issue15_quality.py', 'scripts/issue15_lineage.py', 'scripts/issue20_runtime.py',
         'scripts/issue25_materials.py', 'scripts/issue25_report.py',
         'tests/issue22_retry.py', 'tests/config_abi.py', 'tests/printed_page_integration.py',
         'tests/cli_integration.py', 'tests/pdf_real.py', 'tests/job_control.py',
         'tests/config_integration.py')


def read(path):
    return json.loads(path.read_text())


def suite_specs():
    development = read(MANIFEST)
    annotations = read(DATA / 'OmniDocBench.json')
    verify_data(development, annotations)
    manifest = read(ROOT / MANIFEST_PATH)
    manifest, evaluation_annotations = read_materials(ROOT / MANIFEST_PATH,
        ROOT / manifest['subset_annotation']['path'])
    old = read(ROOT / 'docs/issue-15/samples.json')
    jobs = []
    for group, specs, refs in (('development', development['pages'], annotations),
                              ('evaluation', manifest['pages'], evaluation_annotations),
                              ('old-seven', old['samples'], [None] * len(old['samples']))):
        for spec, annotation in zip(specs, refs):
            source = DATA / spec['image_path'] if group == 'development' else \
                     ROOT / spec['local_path'] if group == 'evaluation' else ROOT / spec['path']
            jobs.append(dict(key=f"{group}/{spec['id']}", source=str(source),
                input_sha256=spec.get('image_sha256', spec.get('sha256')), spec=spec, annotation=annotation))
    return jobs, manifest


def system_runtime(binary, environment):
    """同时记录项目库和系统动态库；PDF 的实际工具及其动态库亦固定。"""
    files = {}
    directory = environment.get('DOCOCR_POPPLER_BIN')
    pdf_tools = [str(Path(directory) / name) if directory else shutil.which(name, path=environment.get('PATH'))
                 for name in ('pdftoppm', 'pdfinfo')]
    if not all(path and Path(path).is_file() and os.access(path, os.X_OK) for path in pdf_tools):
        raise ValueError('真实 PDF 运行时工具缺失')
    for executable in (binary, *(Path(path) for path in pdf_tools)):
        files[str(executable.resolve())] = sha(executable)
        result = subprocess.run(['ldd', str(executable)], env=environment,
                                capture_output=True, text=True, check=True)
        for line in result.stdout.splitlines():
            match = re.search(r'(?:=>\s+)?(/\S+)\s+\(', line)
            if match:
                path = Path(match[1]).resolve()
                files[str(path)] = sha(path)
            if 'not found' in line:
                raise ValueError(f'实际运行时缺失：{line}')
    return files


def freeze_evaluation(output, jobs, manifest, binary, environment):
    files = material_files(manifest)
    for path in (MANIFEST, DATA / 'OmniDocBench.json', ROOT / 'docs/issue-15/samples.json',
                 ROOT / THRESHOLDS_PATH, ROOT / 'configs/printed-page.example.json',
                 ROOT / 'docs/issue-24/report.json', ROOT / 'docs/issue-25/report.json'):
        files[str(path.relative_to(ROOT))] = sha(path)
    for job in jobs:
        source = Path(job['source'])
        if sha(source) != job['input_sha256']:
            raise ValueError(f"原图 SHA 不符：{job['key']}")
        files[str(source.relative_to(ROOT))] = job['input_sha256']
    # 旧回归的参考及 schema 也按实际使用字节固定。
    for directory in ('tests/fixtures', 'docs/issue-15', 'docs/issue-26'):
        for path in (ROOT / directory).rglob('*'):
            if path.is_file() and (path.suffix in ('.json', '.txt', '.py') or 'schema' in path.name):
                files[str(path.relative_to(ROOT))] = sha(path)
    for path in (ROOT / 'docs/issue-24/evidence/development').glob('*/job/document.json'):
        files[str(path.relative_to(ROOT))] = sha(path)
    files.update({name: sha(ROOT / name) for name in TOOLS})
    snapshots = {}
    for name, digest in files.items():
        if Path(name).suffix.lower() in ('.png', '.jpg', '.jpeg', '.pdf'):
            continue
        target = output / 'freeze' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / name, target)
        snapshots[name] = digest
    record = dict(version='issue27-evaluation-1', source_files=files, snapshot_files=snapshots,
        system_runtime_sha256=system_runtime(binary, environment),
        candidate_sha256=sha(output / 'candidate.json'), jobs=jobs,
        expected_jobs=38, expected_pages=39, execution='fresh_public_cli_jobs',
        strict_independent_holdout_claim=False)
    save(output / 'evaluation.json', record)
    return record


def check_frozen(run, entrypoint=None):
    record = read(run / 'evaluation.json')
    jobs, _ = suite_specs()
    if record['jobs'] != jobs or record['expected_jobs'] != 38 or record['expected_pages'] != 39:
        raise ValueError('冻结作业、原参考或完整分母与材料清单不同')
    for name, digest in record['snapshot_files'].items():
        if sha(run / 'freeze' / name) != digest:
            raise ValueError(f'冻结副本 SHA 不符：{name}')
    for name in TOOLS:
        actual = entrypoint if entrypoint and name.endswith('/' + entrypoint.name) else ROOT / name
        if sha(actual) != record['source_files'][name]:
            raise ValueError(f'实际执行工具与冻结版本不同：{name}')
    for name, digest in record['source_files'].items():
        if sha(ROOT / name) != digest:
            raise ValueError(f'材料、参考或工具 SHA 不符：{name}')
    candidate = read(run / 'candidate.json')
    if sha(run / 'candidate.json') != record['candidate_sha256'] or not candidate_intact(run, candidate):
        raise ValueError('候选身份或工件变化')
    for name, digest in candidate['source_sha256'].items():
        if sha(ROOT / name) != digest:
            raise ValueError(f'生产源码与候选不同：{name}')
    binary, config, environment = frozen_runtime(run, candidate)
    if system_runtime(binary, environment) != record['system_runtime_sha256']:
        raise ValueError('实际加载的项目/系统动态库或PDF工具变化')
    return record, candidate, binary, config, environment
