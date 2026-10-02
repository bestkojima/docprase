"""公共 CLI 锁定密封线正文泄漏、标签路由和离线导出的一致性。"""
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

import jsonschema
from PIL import Image
from printed_page_integration import ROOT, config

DEFAULT_IGNORES = ['number', 'footnote', 'header', 'header_image', 'footer', 'footer_image', 'aside_text']
LABELS = ['abstract', 'algorithm', 'aside_text', 'chart', 'content', 'display_formula',
          'doc_title', 'figure_title', 'footer', 'footer_image', 'footnote', 'formula_number',
          'header', 'header_image', 'image', 'inline_formula', 'number', 'paragraph_title',
          'reference', 'reference_content', 'seal', 'table', 'text', 'vertical_text', 'vision_footnote']
SKIP_ORDER = {'figure_title', 'vision_footnote', 'image', 'chart', 'table', 'header',
              'header_image', 'footer', 'footer_image', 'footnote', 'aside_text'}
IMAGES = {3, 9, 13, 14, 20}


def execute(cli, folder, image, setting, trace):
    folder.mkdir()
    cfg, fixture, job = folder / 'config.json', folder / 'fixture.json', folder / 'job'
    cfg.write_text(json.dumps(setting))
    fixture.write_text(json.dumps(trace))
    process = subprocess.run([cli, '--config', str(cfg), '--input', str(image), '--out', str(job)],
                             cwd=ROOT, env=dict(os.environ, DOCOCR_TEST_STRUCTURE_PATH=str(fixture)),
                             capture_output=True, text=True)
    assert process.returncode == 0, process.stderr
    return job, json.loads((job / 'document.json').read_text()), (job / 'document.md').read_text()


def reexport(cli, job, target, document=None, valid=True):
    source = job / 'document.json'
    if document is not None:
        source = target.with_suffix('.json')
        source.write_text(json.dumps(document))
    result = subprocess.run([cli, '--reexport', str(source), '--asset-root', str(job), '--out', str(target)],
                            cwd=ROOT, capture_output=True, text=True)
    assert (result.returncode == 0) == valid, result.stderr
    if valid:
        assert (target / 'document.md').read_bytes() == (job / 'document.md').read_bytes()
        assert (target / 'document.json').read_bytes() == (job / 'document.json').read_bytes()


def main():
    fixture_cli, production_cli = sys.argv[1:3]
    with tempfile.TemporaryDirectory(prefix='dococr-labels-') as tmp:
        root = Path(tmp)
        image = root / 'page.png'
        Image.new('RGB', (800, 1200), 'white').save(image)
        setting = config('printed_page_structure')
        setting['execution'].update(layout_preprocess='reference', layout_score_threshold=.3)
        trace = dict(candidates=[[2, .95, 20, 300, 65, 380, 0],
                                 [22, .95, 160, 300, 700, 380, 1]],
                     outputs=[dict(bbox=[20, 300, 65, 380], text='## 姓名\n'),
                              dict(bbox=[160, 300, 700, 380], text='答卷前，请填写姓名和学号。')])
        job, doc, md = execute(fixture_cli, root / 'seal', image, setting, trace)
        assert '## 姓名' not in md, '密封线 aside_text 混入正文'
        assert '答卷前，请填写姓名和学号。' in md, '正文提及姓名被误删'
        assert doc['schema_version'] == '1.10'
        assert doc['export_policy']['markdown_ignore_labels'] == DEFAULT_IGNORES
        assert json.loads((job / 'execution-plan.json').read_text())['resolved_export_policy'] == doc['export_policy']
        p = doc['pages'][0]
        assert len(p['blocks']) == 2 and len(p['reading_order']) == 2
        aside = next(b for b in p['blocks'] if b['content']['text'] == '## 姓名\n')
        assert aside['status'] == 'ok' and aside['provenance']['raw_output'] == '## 姓名\n'
        assert aside['block_order'] is None
        assert (job / aside['content']['resource']).is_file()
        assert len(json.loads((job / 'run-manifest.json').read_text())['regions']) == 2
        jsonschema.validate(doc, json.loads((ROOT / 'schemas/document-ir/document-ir-1.10-image.schema.json').read_text()))
        reexport(production_cli, job, root / 'seal-reexport')
        # 1.9 历史文档沿用当时的展示语义，不静默应用 1.10 的忽略策略。
        old = copy.deepcopy(doc)
        old['schema_version'] = '1.9'
        old.pop('export_policy')
        for layout in old['pages'][0]['layout_blocks']:
            layout.pop('semantic_label')
        for block in old['pages'][0]['blocks']:
            block.pop('block_order')
        saved = root / 'historical-1.9.json'
        saved.write_text(json.dumps(old))
        result = subprocess.run([production_cli, '--reexport', str(saved), '--asset-root', str(job),
                                 '--out', str(root / 'historical-1.9')], capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        assert '## 姓名' in (root / 'historical-1.9/document.md').read_text()
        for mutation in ('order', 'label', 'policy'):
            bad = copy.deepcopy(doc)
            if mutation == 'order':
                bad['pages'][0]['blocks'][0]['block_order'] = 99
            elif mutation == 'label':
                bad['pages'][0]['layout_blocks'][0]['semantic_label'] = 'text'
            else:
                bad['export_policy']['policy'] = 'unimplemented-policy'
            reexport(production_cli, job, root / ('bad-' + mutation), bad, False)

        # 所有类别单独运行，避免几何合并掩盖标签路由。
        for class_id, label in enumerate(LABELS):
            bounds = [160, 300, 700, 380]
            text = '标签内容_' + label
            if class_id in (5, 15):
                text = 'x=1'
            elif class_id == 21:
                text = '<table><tr><td>标签内容_table</td></tr></table>'
            trace = dict(candidates=[[class_id, .95, *bounds, 0]], outputs=[dict(bbox=bounds, text=text)])
            job, doc, md = execute(fixture_cli, root / str(class_id), image, setting, trace)
            p = doc['pages'][0]
            if class_id == 18:  # Paddle filter_overlap_boxes 同样过滤 reference。
                assert not p['blocks'] and doc['layout_diagnostics']['candidates'][0]['class_id'] == 18
                continue
            b, layout = p['blocks'][0], p['layout_blocks'][0]
            assert layout['original_class_id'] == class_id and layout['semantic_label'] == label
            expected_type = 'image' if class_id in IMAGES else 'formula' if class_id in (5, 15) else 'table' if class_id == 21 else 'text'
            assert b['type'] == expected_type, (label, b['type'])
            hidden = label in DEFAULT_IGNORES or label == 'formula_number'
            assert (md == '') == hidden, (label, md)
            numbered = label not in SKIP_ORDER | set(DEFAULT_IGNORES)
            assert b['block_order'] == (1 if numbered else None), (label, b['block_order'])
            if class_id in IMAGES:
                assert b['provenance']['recognition']['attempts'] == []
                assert (job / b['content']['resource']).is_file()
            else:
                assert b['provenance']['raw_output'] == text and b['status'] == 'ok'
            reexport(production_cli, job, root / ('reexport-' + str(class_id)))

        # 显式空列表恢复全部标签；策略写入 IR，离线导出无需读取原配置。
        setting['execution'].update(markdown_ignore_labels=[], show_formula_number=True)
        trace = dict(candidates=[[2, .95, 20, 300, 65, 380, 0], [11, .95, 160, 500, 240, 550, 1]],
                     outputs=[dict(bbox=[20, 300, 65, 380], text='姓名'), dict(bbox=[160, 500, 240, 550], text='(1)')])
        job, doc, md = execute(fixture_cli, root / 'all-labels', image, setting, trace)
        assert '姓名' in md and '(1)' in md
        assert [b['block_order'] for b in doc['pages'][0]['blocks']] == [None, 1]
        reexport(production_cli, job, root / 'all-labels-reexport')
        # 自定义列表替换默认列表，同时控制编号；未知类别仍输出原图占位。
        setting['execution'].update(markdown_ignore_labels=['text'], show_formula_number=False)
        trace = dict(candidates=[[22, .95, 160, 300, 700, 380, 0], [99, .95, 160, 500, 700, 550, 1]],
                     outputs=[dict(bbox=[160, 300, 700, 380], text='被配置隐藏的正文')])
        job, doc, md = execute(fixture_cli, root / 'custom', image, setting, trace)
        assert '被配置隐藏的正文' not in md and '[未处理：' in md
        assert [b['block_order'] for b in doc['pages'][0]['blocks']] == [None, 1]
        reexport(production_cli, job, root / 'custom-reexport')

        # 旁注识别失败仍影响作业状态，导出隐藏不能伪造成功。
        setting['execution'].pop('markdown_ignore_labels')
        trace = dict(candidates=[[2, .95, 20, 300, 65, 380, 0]],
                     outputs=[dict(bbox=[20, 300, 65, 380], text='', finish_reason='error',
                                   stop_reason='error', error='captured_failure')])
        job, doc, md = execute(fixture_cli, root / 'failed-aside', image, setting, trace)
        assert doc['status'] == 'partial' and doc['pages'][0]['blocks'][0]['status'] == 'failed'
        assert md == ''
        reexport(production_cli, job, root / 'failed-aside-reexport')

        # 配置误拼、重复、错误类型都应在执行前拒绝。
        for i, value in enumerate(['aside_text', ['aside_text', 'aside_text'], ['asid_text'], [22], ['unknown']]):
            bad = copy.deepcopy(setting)
            bad['execution']['markdown_ignore_labels'] = value
            cfg = root / ('invalid-' + str(i) + '.json')
            cfg.write_text(json.dumps(bad))
            result = subprocess.run([fixture_cli, '--config', str(cfg), '--input', str(image),
                                     '--out', str(root / ('invalid-' + str(i)))], cwd=ROOT, capture_output=True, text=True)
            assert result.returncode != 0
            assert json.loads(result.stderr)['code'] == ('type_error' if i in (0, 3) else 'invalid_layout_parameter')
            assert not (root / ('invalid-' + str(i))).exists()

        # 多页 PDF 保留同一策略，每页正文编号从 1 开始；离线导出一致。
        pdf = root / 'pages.pdf'
        frame = Image.new('RGB', (800, 1200), 'white')
        frame.save(pdf, save_all=True, append_images=[frame])
        trace = dict(candidates=[[2, .95, 20, 300, 65, 380, 0], [22, .95, 160, 300, 700, 380, 1]],
                     outputs=[dict(bbox=[20, 300, 65, 380], text='姓名'),
                              dict(bbox=[160, 300, 700, 380], text='PDF正文保留')])
        # CLI 默认 144 dpi，此处指定 72 dpi，确保复现框对应 PDF 栅格。
        folder = root / 'pdf'
        folder.mkdir()
        cfg, fixture, job = folder / 'config.json', folder / 'fixture.json', folder / 'job'
        cfg.write_text(json.dumps(setting))
        fixture.write_text(json.dumps(trace))
        result = subprocess.run([fixture_cli, '--config', str(cfg), '--input', str(pdf), '--out', str(job), '--dpi', '72'],
                                cwd=ROOT, env=dict(os.environ, DOCOCR_TEST_STRUCTURE_PATH=str(fixture)), capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        doc = json.loads((job / 'document.json').read_text())
        md = (job / 'document.md').read_text()
        assert doc['schema_version'] == '1.10' and '姓名' not in md and md.count('PDF正文保留') == 2
        assert all([b['block_order'] for b in p['blocks']] == [None, 1] for p in doc['pages'])
        jsonschema.validate(doc, json.loads((ROOT / 'schemas/document-ir/document-ir-1.10-pdf.schema.json').read_text()))
        reexport(production_cli, job, root / 'pdf-reexport')
        print('label_pipeline: PASS（密封线、25 类路由、保留识别、策略与重导出）')


if __name__ == '__main__':
    main()
