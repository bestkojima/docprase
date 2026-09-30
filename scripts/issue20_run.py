"""运行固定20页的公共 CLI；可只重跑已证实裁图缺少子框边缘的页面。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

from issue17_baseline import DATA, MANIFEST, ROOT, verify_data
from issue20_quality import ownership_audit


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
    binary, config = args.cli.resolve(), args.config.resolve()
    provenance = dict(cli_sha256=sha(binary), config_sha256=sha(config))
    library = binary.parent/'libdococr_c.so'
    if library.exists():
        provenance['library_sha256'] = sha(library)
    # 实验期间仍可编译工作区；运行使用独立快照，避免后续页面加载另一版代码。
    environment = None
    if library.exists() and os.name == 'posix':
        runtime = args.out.resolve()/'.runtime'
        runtime.mkdir(parents=True, exist_ok=True)
        snapshot = runtime/binary.name
        copied_library = runtime/library.name
        if not snapshot.exists():
            shutil.copy2(binary, snapshot)
            shutil.copy2(library, copied_library)
        if sha(snapshot) != provenance['cli_sha256'] or sha(copied_library) != provenance['library_sha256']:
            raise ValueError('已有运行时快照不同，使用新的实验输出目录')
        binary = snapshot
        environment = dict(os.environ, LD_LIBRARY_PATH=str(runtime)+':'+os.environ.get('LD_LIBRARY_PATH', ''))
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
            audit = ownership_audit(document)
            if not audit['owned_crop_truncations']:
                (folder/'job').symlink_to(source, target_is_directory=True)
                record.write_text(json.dumps(dict(execution='reused_unaffected', source=str(source),
                                                  source_sha256=sha(source/'document.json'), provenance=provenance,
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
