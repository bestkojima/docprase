"""真实 Layout/Ovis CPU 双模型经公共 CLI 解析可选文本两页 PDF。"""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time

import jsonschema

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'tests/fixtures/pdf/printed_textbook_2p.pdf'
SOURCE_SHA = '13c682d9d850a23be468cfaec94b136ff67493f8792dac807425561a02cff1d7'
GROUND_TRUTH = [
    ['第一章 分数的意义',
     '分数表示把一个整体平均分成若干份后取其中的一份或几份。',
     '例如，把一张纸平均分成四份，取其中的一份就是四分之一。',
     '分母说明平均分成的份数，分子说明所取的份数。',
     '观察一张图，先找出整体，再数一数阴影部分占几份。'],
    ['第二章 小数与长度',
     '测量一支铅笔的长度时，可以先读出整厘米，再读出不足一厘米的部分。',
     '十个一毫米合成一厘米，因此一点五厘米也可以写成十五毫米。',
     '比较两个长度时，应先统一单位，再比较数值大小。',
     '请写出三件教室物品的长度，并说明选择了什么单位。'],
]


def main():
    binary, output = Path(sys.argv[1]), Path(sys.argv[2])
    assert hashlib.sha256(SOURCE.read_bytes()).hexdigest() == SOURCE_SHA
    output.mkdir(parents=True, exist_ok=True)
    command = [str(binary), '--config', 'configs/printed-page.example.json',
               '--input', str(SOURCE), '--out', str(output / 'job'),
               '--pages', '1-2', '--dpi', '200']
    start = time.monotonic()
    process = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=600)
    wall_seconds = time.monotonic() - start
    (output / 'stdout.log').write_text(process.stdout)
    (output / 'stderr.log').write_text(process.stderr)
    assert process.returncode == 0, (process.returncode, process.stderr)
    job = output / 'job'
    document = json.loads((job / 'document.json').read_text())
    manifest = json.loads((job / 'run-manifest.json').read_text())
    schema = json.loads((ROOT / 'docs/issue-11/document-ir-1.4.schema.json').read_text())
    jsonschema.validate(document, schema)
    assert document['source']['sha256'] == SOURCE_SHA
    assert document['status'] == 'ok'
    assert [p['pdf_page_number'] for p in document['pages']] == [1, 2]
    assert all(p['status'] == 'ok' and p['raster_size'] == [1653, 2339]
               for p in document['pages'])
    observed = []
    for page, expected in zip(document['pages'], GROUND_TRUTH):
        text = [block['content']['text'].strip().removeprefix('## ').strip()
                for block in page['blocks'] if block['type'] == 'text']
        assert text == expected, (page['page_id'], text)
        assert all(block['status'] == 'ok' and block['provenance']['raw_output']
                   for block in page['blocks'])
        observed.append({'page': page['pdf_page_number'], 'blocks': len(page['blocks']),
                         'matched_ground_truth': len(text), 'raster_size': page['raster_size'],
                         'pipeline_ms': next(item['pipeline_ms'] for item in
                                             manifest['pdf']['pages'] if item['page_id'] == page['page_id'])})
    resources = [resource['path'] for resource in document['resources']]
    assert len(resources) == len(set(resources))
    assert all((job / path).is_file() for path in resources)
    assert len(manifest['regions']) == 10
    assert all(region['status'] == 'ok' and region['stop_reason'] == 'normal'
               for region in manifest['regions'])
    markdown = (job / 'document.md').read_text()
    assert markdown.index('第一章') < markdown.index('第二章')
    summary = {'command': command, 'exit_code': process.returncode,
               'source_sha256': SOURCE_SHA, 'source_kind': 'project_authored_selectable_text_pdf',
               'dpi': 200, 'selected_pages': [1, 2], 'wall_seconds_python': round(wall_seconds, 3),
               'status': document['status'], 'pages': observed, 'resources': len(resources),
               'manifest_total_wall_ms': manifest['pdf']['total_wall_ms'],
               'renderer_version': manifest['pdf']['renderer_version']}
    (output / 'summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(summary, ensure_ascii=False))


if __name__ == '__main__':
    main()
