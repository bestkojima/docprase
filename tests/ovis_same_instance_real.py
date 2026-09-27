"""在一个 Ovis 实例中验证 reset 隔离；生成正式可复跑的原始输出证据。"""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
MODEL = ROOT / 'models/ovis'
FIXTURES = ROOT / 'tests/fixtures/ovis'


def main():
    binary, destination = Path(sys.argv[1]), Path(sys.argv[2])
    destination.mkdir(parents=True, exist_ok=True)
    config = json.loads((MODEL / 'config.json').read_text())
    config.update(base_dir=str(MODEL.resolve()) + '/', sampler_type='greedy',
                  reuse_kv=False, prompt_cache=False, use_mmap=False,
                  kvcache_mmap=False, **{'async': False, 'thread_num': 1, 'timeout_ms': 120000})
    config['mllm']['thread_num'] = 1
    effective = destination / 'effective-config.json'
    effective.write_text(json.dumps(config, ensure_ascii=False, indent=2) + '\n')
    paths = [str((FIXTURES / name).resolve()) for name in
             ('complete_table.png', 'chinese_text.png', 'invalid.png')]
    command = [str(binary), str(effective), str(FIXTURES / 'prompt.txt'), str(destination), *paths]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=600)
    (destination / 'stdout.log').write_text(result.stdout)
    (destination / 'stderr.log').write_text(result.stderr)
    loaded = json.loads((destination / 'loaded-config.json').read_text())
    assert loaded['backend_type'] == 'cpu' and loaded['thread_num'] == 1
    assert loaded['mllm']['backend_type'] == 'cpu' and loaded['mllm']['thread_num'] == 1
    assert loaded['sampler_type'] == 'greedy' and loaded['reuse_kv'] is False
    assert loaded['prompt_cache'] is False and loaded['timeout_ms'] == 120000
    lines = [json.loads(line.removeprefix('OVIS_SAME_INSTANCE ')) for line in result.stdout.splitlines()
             if line.startswith('OVIS_SAME_INSTANCE ')]
    hashes = [hashlib.sha256((destination / f'raw-{i}.txt').read_bytes()).hexdigest()
              for i in range(len(lines))]
    report = {'command': command, 'exit_code': result.returncode, 'runs': lines,
              'raw_sha256': hashes, 'shared_instance': True, 'thread_num': 1}
    (destination / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    assert result.returncode == 0 and len(lines) == 5, report
    assert all(lines[i]['status'] == 'NORMAL_FINISHED' and
               lines[i]['vision_us'] > 0 and lines[i]['pixels_mp'] > 0 for i in (0, 1, 2, 4))
    assert hashes[0] == hashes[2] == hashes[4], report
    assert lines[3]['vision_us'] == 0 and lines[3]['pixels_mp'] == 0
    print(json.dumps({'pass': True, 'b_sha256': hashes[0], 'bad_status': lines[3]['status']}))


if __name__ == '__main__':
    main()
