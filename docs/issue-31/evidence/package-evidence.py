"""#31 证据打包：原始字节不改写，模型权重只记录原冻结身份。"""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[3]
RUN = ROOT / 'output/issue31-final-20261003'
DIAGNOSTIC = ROOT / 'output/issue31-margin-production-20261003'
EVIDENCE = ROOT / 'docs/issue-31/evidence'
ARCHIVES = ROOT / 'output/issue31-archives-20261003'


def sha(path):
    return hashlib.file_digest(path.open('rb'), 'sha256').hexdigest()


def read(path):
    return json.loads(path.read_text())


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')


def archive(name, source, files, role):
    target = ARCHIVES / name
    members = {}
    with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as output:
        for path in sorted(files):
            key = str(path.relative_to(source))
            members[key] = sha(path)
            output.write(path, key)
    with zipfile.ZipFile(target) as saved:
        assert set(saved.namelist()) == set(members)
        for name, digest in members.items():
            assert hashlib.sha256(saved.read(name)).hexdigest() == digest
    print(target.name, len(members), flush=True)
    return dict(path=str(target.relative_to(ROOT)), sha256=sha(target), bytes=target.stat().st_size,
                role=role, members_sha256=members)


def main():
    ARCHIVES.mkdir(parents=True, exist_ok=True)
    report = read(RUN / 'issue31-report.json')
    evaluation = read(RUN / 'evaluation.json')
    archives = []
    pages = {}
    specs = {s['key']: s for s in evaluation['jobs']}
    for key, spec in specs.items():
        folder = RUN / key
        archives.append(archive(key.replace('/', '-') + '.zip', folder,
            [p for p in folder.rglob('*') if p.is_file()], 'fresh_frozen_candidate'))
        document = read(folder / 'job/document.json')
        group, page_id = key.split('/')
        score = report['groups'][group]['pages'][page_id] if group in report['groups'] else None
        trace = dict(key=key, input=dict(path=spec['source'], sha256=spec['input_sha256']),
            command=read(folder / 'command.json'), document_status=document['status'],
            schema_version=document['schema_version'], reference=spec['annotation'],
            pages=document['pages'], resources=document['resources'],
            markdown=(folder / 'job/document.md').read_text(),
            quality=score['parent'] if score else report['old_seven']['samples'][page_id]['strict_after'],
            content_audit=score['content_audit'] if score else dict(verified=False,
                blockers=['旧回归仅核验已有参考子集，不声明整页逐字正确']),
            reference_plan=score['reference_plan'] if score else None,
            alignment=[{k:v for k,v in a.items() if k not in ('operations','raw_operations')}
                       for a in score['alignment']] if score else [],
            non_text_evidence=score['non_text_evidence'] if score else [],
            inline_formula_diagnostics=score['inline_formula_diagnostics'] if score else [],
            full_character_alignment=str((RUN/'issue31-report.json').relative_to(ROOT)))
        name = key.replace('/', '-') + '.json'
        save(EVIDENCE / 'pages' / name, trace)
        text = score['parent']['text'] if score else None
        pages[key] = dict(evidence='pages/'+name,
            reference_chars=text['reference_chars'] if text else None,
            edit_distance=text['edit_distance'] if text else None,
            content_verified=trace['content_audit']['verified'],
            blockers=trace['content_audit']['blockers'])
    files = [p for p in RUN.rglob('*') if p.is_file() and
             p.relative_to(RUN).parts[0] not in ('development','evaluation','old-seven') and
             not str(p.relative_to(RUN)).startswith('candidate/models/')]
    archives.append(archive('frozen-runtime-and-report.zip',RUN,files,'accepted_engineering_quality_failed'))
    # 撤回候选的实际源码从其提交恢复；预存未提交文件必须匹配已保存 SHA。
    candidate = read(DIAGNOSTIC / 'candidate.json')
    for name, digest in candidate['source_sha256'].items():
        content = subprocess.check_output(['git','show',candidate['code_commit']+':'+name],cwd=ROOT)
        if hashlib.sha256(content).hexdigest() != digest:
            content = (ROOT/name).read_bytes()
        assert hashlib.sha256(content).hexdigest() == digest
        target = DIAGNOSTIC/'source-snapshot'/name
        target.parent.mkdir(parents=True,exist_ok=True); target.write_bytes(content)
    files = [p for p in DIAGNOSTIC.rglob('*') if p.is_file() and
             not str(p.relative_to(DIAGNOSTIC)).startswith('candidate/models/')]
    archives.append(archive('withdrawn-margin-candidate.zip',DIAGNOSTIC,files,'rejected_real_regressions'))
    interrupted = ROOT/'output/issue31-full-20261003'
    files = [p for p in interrupted.rglob('*') if p.is_file() and
             not str(p.relative_to(interrupted)).startswith('candidate/models/')]
    archives.append(archive('interrupted-attempt.zip',interrupted,files,'sigkill_not_acceptance'))
    for name in ('candidate.json','evaluation.json','run-summary.json','integrity-before.json',
                 'issue31-export-verification.json','automated-verification.json',
                 'safety-verification.json','source-state.json','working-source.patch'):
        shutil.copy2(RUN/name,EVIDENCE/name)
    shutil.copy2(RUN/'safety/summary.json',EVIDENCE/'real-safety-summary.json')
    shutil.copy2(DIAGNOSTIC/'comparison.json',EVIDENCE/'margin-production-comparison.json')
    shutil.copy2(DIAGNOSTIC/'candidate.json',EVIDENCE/'withdrawn-margin-candidate.json')
    summary = {k:v for k,v in report.items() if k not in ('groups','old_seven')}
    summary['groups'] = {name:{k:v for k,v in group.items() if k not in ('pages','baseline','changes')}
        for name,group in report['groups'].items()}
    summary['historical_output_rescores'] = {name:dict(execution='historical_output_rescore',
        aggregate=g['baseline']['aggregate'], metrics=g['baseline']['metrics']) for name,g in report['groups'].items()}
    summary['old_seven_blockers'] = report['old_seven']['blockers']
    summary['full_report'] = dict(path=str((RUN/'issue31-report.json').relative_to(ROOT)),
                                 sha256=sha(RUN/'issue31-report.json'))
    summary['pages'] = pages
    save(EVIDENCE/'summary.json',summary)
    save(EVIDENCE/'artifacts.json',dict(version='issue31-raw-artifacts-1',
        transformation='byte_preserving_archive_and_copy_only',
        packaging_tool_sha256=sha(Path(__file__)), candidate_sha256=report['candidate_sha256'],
        evaluation_sha256=report['evaluation_sha256'], model_weights='未纳入 ZIP；原冻结目录及 candidate.json 保留身份',
        archives=archives))


if __name__ == '__main__':
    main()
