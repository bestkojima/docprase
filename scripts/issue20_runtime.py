"""为对照实验冻结可执行文件和项目动态库，记录实际加载来源。"""
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def runtime_sources(binary, environment=None):
    binary = binary.resolve()
    sources = {binary.name: binary}
    if os.name != 'posix' or not shutil.which('ldd'):
        raise RuntimeError('当前实验运行时冻结要求 Linux ldd')
    result = subprocess.run(['ldd', str(binary)], env=environment, capture_output=True, text=True, check=True)
    for line in result.stdout.splitlines():
        match = re.match(r'\s*(lib(?:MNN[^\s]*|llm|dococr[^\s]*)\.so[^\s]*) => (\S+)', line)
        if match:
            path = Path(match[2]).resolve()
            if not path.is_file():
                raise ValueError(f'运行时库缺失：{line}')
            sources[match[1]] = path
    return sources


def freeze_runtime(binary, directory):
    sources = runtime_sources(binary)
    provenance = {name: dict(source=str(path), sha256=sha(path)) for name, path in sources.items()}
    directory.mkdir(parents=True, exist_ok=True)
    for name, source in sources.items():
        target = directory/name
        if not target.exists():
            shutil.copy2(source, target)
        if sha(target) != provenance[name]['sha256']:
            raise ValueError(f'已有运行时快照不同，使用新输出目录：{target}')
    environment = dict(os.environ, LD_LIBRARY_PATH=str(directory)+':'+os.environ.get('LD_LIBRARY_PATH', ''))
    loaded = runtime_sources(directory/binary.name, environment)
    if set(loaded) != set(sources) or any(path != (directory/name).resolve() for name, path in loaded.items()):
        raise ValueError('实际动态库加载路径没有全部指向快照')
    return directory/binary.name, environment, provenance
