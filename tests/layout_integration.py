"""从公共 CLI/作业 ABI 验证真实 Layout 契约的受控边界行为。"""
import copy
import json
import pathlib
import struct
import subprocess
import sys
import tempfile

from cli_integration import png_2x2

ROOT = pathlib.Path(__file__).resolve().parents[1]
SCHEMA = json.loads((ROOT / 'docs/issue-10/document-ir-1.3.schema.json').read_text())


def config(backend):
    template = json.loads((ROOT / 'configs/layout-plan.example.json').read_text())
    template['mode'] = 'development'
    template['backend'] = backend
    template['models']['layout'] = copy.deepcopy(json.loads(
        (ROOT / 'configs/fixture-plan.example.json').read_text())['models']['layout'])
    return template


def run(binary, cfg, image, out):
    cfg_path = out.parent / (out.name + '.json')
    cfg_path.write_text(json.dumps(cfg, ensure_ascii=False))
    return subprocess.run([binary, '--config', str(cfg_path), '--input', str(image), '--out', str(out)],
                          cwd=ROOT, capture_output=True, text=True)


def decode_masks(data):
    magic = b'DOCOCR_MASK_RLE_V1\n'
    assert data.startswith(magic)
    pos = len(magic)
    rows, height, width = struct.unpack_from('<III', data, pos)
    pos += 12
    assert (rows, height, width) == (300, 200, 200)
    result = []
    for _ in range(rows):
        initial, count = struct.unpack_from('<II', data, pos)
        pos += 8
        lengths = struct.unpack_from('<' + 'I'*count, data, pos)
        pos += 4*count
        assert sum(lengths) == 40000
        assert initial in (0, 1)
        result.append(sum(length for i, length in enumerate(lengths) if (initial + i) % 2))
    assert pos == len(data)
    return result


def main():
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix='dococr-layout-') as tmp:
        root = pathlib.Path(tmp)
        image = root / 'page.png'
        image.write_bytes(png_2x2())
        out = root / 'controlled'
        completed = run(binary, config('fixture:layout_contract'), image, out)
        assert completed.returncode == 0, (completed.returncode, completed.stderr)
        document = json.loads((out / 'document.json').read_text())
        page = document['pages'][0]
        diagnostics = document['layout_diagnostics']
        assert document['status'] == page['status'] == 'partial'
        assert diagnostics['candidate_count'] == 7
        assert [c['candidate_id'] for c in diagnostics['candidates'] if c['selected']] == [0, 1, 2]
        assert [c['filter_reason'] for c in diagnostics['candidates'][3:]] == [
            'below_score_threshold', 'outside_page', 'degenerate_box', 'box_out_of_supported_range']
        assert diagnostics['candidates'][0]['clamped'] is True
        assert diagnostics['candidates'][2]['handling_reason'] == 'unknown_class'
        assert [block['candidate_id'] for block in page['layout_blocks']] == [0, 1, 2]
        assert [block['candidate_rank'] for block in page['layout_blocks']] == [7, 7, 9]
        assert len({block['id'] for block in page['layout_blocks']}) == 3
        assert page['layout_blocks'][0]['bbox'] == [0, 0, 2, 2]
        assert page['layout_blocks'][1]['bbox'] == [0, 0, 1, 1]  # 合法父子包含关系
        assert [block['status'] for block in page['blocks']] == ['skipped']*3
        assert all(block['content']['resource'] and block['content']['text'] == '' for block in page['blocks'])
        assert page['blocks'][2]['error'] == 'unknown_layout_class'
        assert len(page['reading_order']) == 3
        assert len(list((out / 'assets').iterdir())) == 13  # 6 张量、3 区域、3 mask、1 叠加图
        names = diagnostics['raw_tensor_assets']
        assert len((out / names['image']).read_bytes()) == 3*800*800*4
        assert struct.unpack('<2f', (out / names['im_shape']).read_bytes()) == (800., 800.)
        assert struct.unpack('<2f', (out / names['scale_factor']).read_bytes()) == (400., 400.)
        assert len((out / names['fetch_name_0']).read_bytes()) == 300*7*4
        assert struct.unpack('<i', (out / names['fetch_name_1']).read_bytes()) == (7,)
        assert decode_masks((out / names['fetch_name_2']).read_bytes())[:7] == [1, 1, 1, 0, 0, 0, 0]
        for candidate in diagnostics['candidates'][:3]:
            assert (out / candidate['mask_asset']).read_bytes().startswith(b'\x89PNG')
        assert (out / diagnostics['overlay_asset']).read_bytes().startswith(b'\x89PNG')
        import jsonschema
        jsonschema.validate(document, SCHEMA)
        manifest = json.loads((out / 'run-manifest.json').read_text())
        assert manifest['timing_status']['recognition'] == 'not_run'
        assert manifest['processing'][4]['status'] == 'skipped_disabled'

        inline_out = root / 'inline-layout-only'
        completed = run(binary, config('fixture:layout_inline_formula'), image, inline_out)
        assert completed.returncode == 0, completed.stderr
        inline_doc = json.loads((inline_out / 'document.json').read_text())
        jsonschema.validate(inline_doc, SCHEMA)
        assert inline_doc['schema_version'] == '1.3'
        assert [block['type'] for block in inline_doc['pages'][0]['blocks']] == [
            'text', 'formula']
        assert inline_doc['pages'][0]['relations'] == []
        assert [candidate['candidate_id'] for candidate in
                inline_doc['layout_diagnostics']['candidates'] if candidate['selected']] == [0, 1]
        assert all((inline_out / candidate['mask_asset']).exists() for candidate in
                   inline_doc['layout_diagnostics']['candidates'] if candidate['selected'])

        empty = root / 'empty'
        completed = run(binary, config('fixture:layout_empty'), image, empty)
        assert completed.returncode == 0, completed.stderr
        blank = json.loads((empty / 'document.json').read_text())
        assert blank['status'] == 'blank' and blank['pages'][0]['blocks'] == []
        assert blank['layout_diagnostics']['candidate_count'] == 0
        assert decode_masks((empty / blank['layout_diagnostics']['raw_tensor_assets']['fetch_name_2']).read_bytes()) == [0]*300
        jsonschema.validate(blank, SCHEMA)

        failed = root / 'inference-failed'
        completed = run(binary, config('fixture:layout_infer_failure'), image, failed)
        assert completed.returncode == 3 and 'layout_inference_failed' in completed.stderr
        event = json.loads(completed.stderr.splitlines()[0])
        assert event['error']['stage'] == 'layout'
        manifest = json.loads((failed / 'run-manifest.json').read_text())
        assert manifest['job_status'] == 'failed'
        assert manifest['failure_code'] == 'layout_inference_failed'
        assert 'controlled' in manifest['failure_detail']
        assert not (failed / 'document.json').exists()


if __name__ == '__main__':
    main()
