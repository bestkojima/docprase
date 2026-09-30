"""运行固定20页的公共 CLI；可只重跑已证实裁图缺少子框边缘的页面。"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

from issue17_baseline import DATA, MANIFEST, ROOT, verify_data
from issue20_quality import ownership_audit
from issue20_runtime import freeze_runtime


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli', type=Path, required=True)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--repair-crops-from', type=Path)
    args = parser.parse_args()
    manifest = json.loads(MANIFEST.read_text())
    annotations = json.loads((DATA/'OmniDocBench.json').read_text())
    verify_data(manifest, annotations)
    config_bytes = args.config.resolve().read_bytes()
    runtime = args.out.resolve()/'.runtime'
    binary, environment, libraries = freeze_runtime(args.cli.resolve(), runtime)
    config = runtime/'config.json'
    if config.exists() and config.read_bytes() != config_bytes:
        raise ValueError('已有配置快照不同，使用新输出目录')
    config.write_bytes(config_bytes)
    provenance = dict(runtime=libraries, config_sha256=sha(config))
    # 与生产配置规范化哈希严格比较；非标准数字书写可能保守地拒绝复用。
    normalized = json.dumps(json.loads(config_bytes), ensure_ascii=False, sort_keys=True, separators=(',', ':'))
    config_hash = hashlib.sha256(normalized.encode()).hexdigest()
    for spec in manifest['pages']:
        folder = args.out.resolve()/spec['id']
        folder.mkdir(parents=True, exist_ok=True)
        record = folder/'command.json'
        if record.exists():
            saved = json.loads(record.read_text())
            if saved.get('provenance') != provenance:
                raise ValueError(f'已有实验的执行来源不同：{record}')
            if saved.get('returncode') != 0:
                raise ValueError(f'已有失败需要单独调查，不静默跳过：{record}')
            continue
        source = args.repair_crops_from.resolve()/spec['id']/'job' if args.repair_crops_from else None
        if source:
            document = json.loads((source/'document.json').read_text())
            source_manifest = json.loads((source/'run-manifest.json').read_text())
            if source_manifest['config_hash'] != config_hash:
                raise ValueError(f'修复来源的配置/模型不一致，禁止复用：{source}')
            audit = ownership_audit(document)
            if not audit['owned_crop_truncations']:
                (folder/'job').symlink_to(source, target_is_directory=True)
                record.write_text(json.dumps(dict(execution='reused_unaffected', source=str(source),
                                                  source_sha256=sha(source/'document.json'), provenance=provenance,
                                                  reused_run_manifest=source_manifest,
                                                  returncode=0), ensure_ascii=False, indent=2)+'\n')
                print(spec['id'], 'reused_unaffected', flush=True)
                continue
        command = [str(binary), '--config', str(config), '--input', str(DATA/spec['image_path']),
                   '--out', str(folder/'job')]
        started = time.monotonic()
        result = subprocess.run(command, cwd=ROOT, env=environment, capture_output=True, text=True)
        (folder/'stdout.log').write_text(result.stdout)
        (folder/'stderr.log').write_text(result.stderr)
        record.write_text(json.dumps(dict(command=command, provenance=provenance,
                                          input_sha256=spec['image_sha256'], returncode=result.returncode,
                                          elapsed_seconds=time.monotonic()-started), ensure_ascii=False, indent=2)+'\n')
        print(spec['id'], result.returncode, round(time.monotonic()-started), flush=True)
        if result.returncode:
            raise RuntimeError(result.stderr)
        if source:
            repaired = ownership_audit(json.loads((folder/'job/document.json').read_text()))
            if repaired['errors'] or repaired['owned_crop_truncations']:
                raise ValueError(f'裁图归属修复未通过：{folder}')


if __name__ == '__main__':
    main()
