"""Run frozen #15 inputs through the production CLI and check export invariants."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

import jsonschema
from PIL import Image
from issue15_lineage import lineage_failures


ROOT = Path(__file__).resolve().parents[1]
SAMPLES = json.loads((ROOT / 'docs/issue-15/samples.json').read_text())


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, folder):
    folder.mkdir(parents=True, exist_ok=True)
    wrapped = ['/usr/bin/time', '-v', '-o', str(folder / 'time.txt'), *command]
    result = subprocess.run(wrapped, cwd=ROOT, text=True, capture_output=True,
                            timeout=1800)
    (folder / 'stdout.log').write_text(result.stdout)
    (folder / 'stderr.log').write_text(result.stderr)
    (folder / 'command.json').write_text(json.dumps({
        'command': command, 'returncode': result.returncode}, ensure_ascii=False,
        indent=2) + '\n')
    return result.returncode


def local_assets(document):
    paths = [r['path'] for r in document['resources']]
    def find(value):
        if isinstance(value, dict):
            for key, child in value.items():
                if isinstance(child, str) and child.startswith('assets/') and key != 'raw_output':
                    paths.append(child)
                else:
                    find(child)
        elif isinstance(value, list):
            for child in value:
                find(child)
    find(document.get('layout_diagnostics', {}))
    for page in document['pages']:
        find(page.get('layout_diagnostics', {}))
    return sorted(set(paths))


def check_document(job, sample):
    document = json.loads((job / 'document.json').read_text())
    schema = ROOT / ('docs/issue-11/document-ir-1.4.schema.json' if
                     document['schema_version'] == '1.4' else
                     'docs/issue-10/document-ir-1.3.schema.json')
    jsonschema.validate(document, json.loads(schema.read_text()))
    failures = []
    jpeg_crop_max_abs = None
    paths = local_assets(document)
    for path in paths:
        asset = (job / path).resolve()
        if not asset.is_relative_to(job.resolve()) or not asset.is_file():
            failures.append(f'missing_or_outside_asset:{path}')
    resource_paths = [r['path'] for r in document['resources']]
    resource_by_path = {r['path']: r for r in document['resources']}
    if len(resource_paths) != len(set(resource_paths)):
        failures.append('duplicate_resource_path')
    source = ROOT / sample['path']
    original = None
    if source.suffix != '.pdf':
        original = Image.open(source).convert('RGB')
    for page in document['pages']:
        blocks = page['blocks']
        ids = [b['id'] for b in blocks]
        if len(ids) != len(set(ids)) or sorted(ids) != sorted(page['reading_order']):
            failures.append(f"{page['page_id']}:invalid_reading_order")
        layout = {b['id']: b for b in page['layout_blocks']}
        regions = {r['id']: r for r in page['regions']}
        block_by_id = {b['id']: b for b in blocks}
        width, height = page['raster_size']
        if len(layout) != len(page['layout_blocks']) or len(regions) != len(page['regions']):
            failures.append(f"{page['page_id']}:duplicate_layout_or_region_id")
        failures.extend(f"{page['page_id']}:{failure}" for failure in lineage_failures(page))
        for region in page['regions']:
            if any(layout_id not in layout for layout_id in region['source_layout_block_ids']):
                failures.append(f"{region['id']}:missing_layout_block")
        if source.suffix == '.pdf':
            affine = page.get('pdf_points_to_raster_affine')
            if not isinstance(affine, list) or len(affine) != 6 or not all(
                    isinstance(x, (int, float)) for x in affine):
                failures.append(f"{page['page_id']}:invalid_pdf_affine")
        for b in blocks:
            x0, y0, x1, y1 = b['bbox']
            if not (0 <= x0 < x1 <= width and 0 <= y0 < y1 <= height):
                failures.append(f"{b['id']}:out_of_bounds")
            if any(r not in regions for r in b['source_region_ids']):
                failures.append(f"{b['id']}:missing_region")
            if b.get('confidence') is not None:
                failures.append(f"{b['id']}:invented_recognition_confidence")
            resource = b['content'].get('resource')
            if resource and (resource not in resource_by_path or
                             resource_by_path[resource].get('source_block_id') != b['id']):
                failures.append(f"{b['id']}:resource_owner_mismatch")
            if original is not None and b['content'].get('resource'):
                with Image.open(job / b['content']['resource']) as crop:
                    observed = crop.convert('RGB')
                    reference = original.crop(tuple(b['bbox']))
                    if observed.size != reference.size:
                        failures.append(f"{b['id']}:crop_size_mismatch")
                    elif source.suffix.lower() in ('.jpg', '.jpeg'):
                        # The production stb JPEG decoder differs from Pillow by at most
                        # one channel value on the existing #6 reference page.
                        maximum = max(abs(a - b) for a, b in zip(
                            observed.tobytes(), reference.tobytes()))
                        jpeg_crop_max_abs = max(jpeg_crop_max_abs or 0, maximum)
                        if maximum > 1:
                            failures.append(f"{b['id']}:jpeg_crop_max_abs:{maximum}")
                    elif observed.tobytes() != reference.tobytes():
                        failures.append(f"{b['id']}:crop_mismatch")
        for rel in page['relations']:
            if rel['type'] == 'content_owned_by':
                child = layout.get(rel['source_layout_block_id'])
                owner = block_by_id.get(rel['owner_block_id'])
                if not child or not owner:
                    failures.append('broken_ownership_reference')
                elif not (owner['bbox'][0] <= child['bbox'][0] <= child['bbox'][2] <= owner['bbox'][2]
                          and owner['bbox'][1] <= child['bbox'][1] <= child['bbox'][3] <= owner['bbox'][3]):
                    failures.append(f"{child['id']}:owner_box_does_not_contain_child")
            elif rel.get('source_block_id') not in block_by_id or rel.get('target_block_id') not in block_by_id:
                failures.append('broken_relation_reference')
    markdown = (job / 'document.md').read_text()
    for ref in re.findall(r'(?:!\[[^\]]*\]\(|src=")[^\"]*(assets/[^)\"]+)', markdown):
        if not (job / ref).is_file():
            failures.append(f'missing_markdown_asset:{ref}')
    if original is not None:
        original.close()
    return document, paths, failures, jpeg_crop_max_abs


def check_reexport(cli, job, copied, paths):
    code = run([str(cli), '--reexport', str(job / 'document.json'),
                '--asset-root', str(job), '--out', str(copied)], copied.parent / 'reexport-run')
    failures = []
    if code:
        failures.append(f'reexport_exit:{code}')
        return failures
    return verify_reexport_files(job, copied, paths)


def verify_reexport_files(job, copied, paths):
    failures = []
    for name in ['document.json', 'document.md', *paths]:
        if not (copied / name).is_file() or sha(job / name) != sha(copied / name):
            failures.append(f'reexport_mismatch:{name}')
    return failures


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cli', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--verify-only', action='store_true',
                        help='只复核已有作业与再导出文件，不重新运行推理')
    args = parser.parse_args()
    cli = args.cli.resolve()
    output = args.out.resolve()
    if args.verify_only:
        if not output.is_dir():
            raise ValueError('复核需要已有输出目录')
    else:
        output.mkdir(parents=True, exist_ok=False)
    report = {'baseline_commit': SAMPLES['baseline_commit'], 'samples': {},
              'hard_failures': []}
    for sample in SAMPLES['samples']:
        source = ROOT / sample['path']
        if sha(source) != sample['sha256']:
            raise ValueError(f"样本哈希不匹配：{sample['id']}")
        folder = output / sample['id']
        job = folder / 'job'
        command = [str(cli), '--config', 'configs/printed-page.example.json',
                   '--input', str(source), '--out', str(job)]
        if source.suffix == '.pdf':
            command += ['--pages', '1-2', '--dpi', '200']
        code = (json.loads((folder / 'command.json').read_text())['returncode']
                if args.verify_only else run(command, folder))
        if code:
            report['samples'][sample['id']] = {'exit_code': code, 'failures': [f'cli_exit:{code}']}
            report['hard_failures'].append(f"{sample['id']}:cli_exit:{code}")
        else:
            document, paths, failures, jpeg_crop_max_abs = check_document(job, sample)
            if args.verify_only:
                failures += verify_reexport_files(job, folder / 'reexport', paths)
            else:
                failures += check_reexport(cli, job, folder / 'reexport', paths)
            report['samples'][sample['id']] = {'exit_code': code,
                'document_status': document['status'], 'pages': len(document['pages']),
                'blocks': sum(len(p['blocks']) for p in document['pages']),
                'assets': len(paths), 'jpeg_crop_max_abs': jpeg_crop_max_abs,
                'failures': failures,
                'document_sha256': sha(job / 'document.json'),
                'manifest_sha256': sha(job / 'run-manifest.json')}
            report['hard_failures'] += [f"{sample['id']}:{f}" for f in failures]
        (output / 'progress.json').write_text(json.dumps(report, ensure_ascii=False,
                                                       indent=2) + '\n')
        print(sample['id'], report['samples'][sample['id']], flush=True)
    (output / 'summary.json').write_text(json.dumps(report, ensure_ascii=False,
                                             indent=2) + '\n')
    return int(bool(report['hard_failures']))


if __name__ == '__main__':
    sys.exit(main())
