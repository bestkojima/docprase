"""通过公共 CLI 重放密封线误入正文；这是诊断用例，不执行新模型推理。"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests"))
from PIL import Image
from printed_page_integration import config


def read(path):
    return json.loads(path.read_text())


def write(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=["original", "minimal", "without-name"], default="original")
    parser.add_argument("--cli", type=Path, default=ROOT / "build/linux-current/dococr_cli_fixture")
    parser.add_argument("--production-cli", type=Path, default=ROOT / "build/linux-current/dococr_cli")
    parser.add_argument("--out", type=Path)
    parser.add_argument("--class-id", type=int, help="仅用于最小复现：只改变模型类别")
    parser.add_argument("--move-to-body", action="store_true", help="仅用于最小复现：只将同一块移入正文")
    parser.add_argument("--plain-name", action="store_true", help="仅用于最小复现：只移除模型输出中的标题符号")
    args = parser.parse_args()
    if args.mode != "minimal" and (args.class_id is not None or args.move_to_body or args.plain_name):
        parser.error("探针只适用于 --mode minimal")
    output = args.out or ROOT / "output/seal-margin-diagnosis" / str(time.time_ns())
    output.mkdir(parents=True, exist_ok=False)
    specs = {p["id"]: p for p in read(ROOT / "docs/omnidocbench-20/manifest.json")["pages"]}
    names = ["odb-01", "odb-02"] if args.mode == "original" else ["odb-01"]
    results = []
    for name in names:
        folder = output / name
        folder.mkdir()
        source = ROOT / "output/issue-27/development" / name / "job"
        document = read(source / "document.json")
        page = document["pages"][0]
        setting = config("printed_page_structure")
        if args.mode == "original":
            effective = read(source / "execution-plan.json")["effective_config"]
            setting["execution"] = effective["execution"]
            setting["platform"] = effective["platform"]
            image = ROOT / "output/omnidocbench/selected-20" / specs[name]["image_path"]
            assert image.exists(), image
            assert sha(image) == specs[name]["image_sha256"], "原图哈希不符"
            diagnostics = document["layout_diagnostics"]
            trace = dict(candidate_tensor_path=str(source / diagnostics["raw_tensor_assets"]["fetch_name_0"]),
                         candidate_count=diagnostics["candidate_count"],
                         mask_rle_path=str(source / diagnostics["raw_tensor_assets"]["fetch_name_2"]), outputs=[])
            for block in page["blocks"]:
                if block["type"] not in ("text", "formula", "table"):
                    continue
                recognition = block["provenance"]["recognition"]
                attempt = next(a for a in recognition["attempts"]
                               if a["index"] == recognition["selected_attempt"])
                value = copy.deepcopy(attempt["output"])
                value.update(bbox=block["bbox"], error=value.get("error") or "")
                trace["outputs"].append(value)
        else:
            image = folder / "page.png"
            Image.new("RGB", tuple(page["raster_size"]), "white").save(image)
            block = next(b for b in page["blocks"] if b["id"] == "b0041")
            region = next(r for r in page["regions"] if r["id"] == block["source_region_ids"][0])
            layout = next(l for l in page["layout_blocks"] if l["id"] == region["source_layout_block_ids"][0])
            setting["execution"].update(layout_preprocess="reference", layout_score_threshold=.3)
            trace = dict(candidates=[], outputs=[])
            if args.mode == "minimal":
                trace["candidates"].append([layout["original_class_id"], layout["detection_score"],
                                            *layout["original_bbox"], 0])
                trace["outputs"].append(dict(bbox=block["bbox"], text=block["provenance"]["raw_output"]))
                if args.class_id is not None:
                    trace["candidates"][0][0] = args.class_id
                if args.move_to_body:
                    for index in [2, 4]:
                        trace["candidates"][0][index] += 700
                    trace["outputs"][0]["bbox"] = [v + (700 if i in [0, 2] else 0)
                                                    for i, v in enumerate(block["bbox"])]
                if args.plain_name:
                    trace["outputs"][0]["text"] = "姓名\n"
        write(folder / "config.json", setting)
        write(folder / "fixture.json", trace)
        command = [str(args.cli.resolve()), "--config", str(folder / "config.json"),
                   "--input", str(image), "--out", str(folder / "job")]
        start = time.monotonic()
        process = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
            env=dict(os.environ, DOCOCR_TEST_STRUCTURE_PATH=str(folder / "fixture.json")), timeout=30)
        assert process.returncode == 0, process.stderr
        actual = read(folder / "job/document.json")
        markdown = (folder / "job/document.md").read_text()
        exported = subprocess.run([str(args.production_cli.resolve()), "--reexport",
            str(folder / "job/document.json"), "--asset-root", str(folder / "job"),
            "--out", str(folder / "reexport")], cwd=ROOT, capture_output=True, text=True, timeout=30)
        assert exported.returncode == 0, exported.stderr
        for filename in ["document.md", "document.json"]:
            assert (folder / "reexport" / filename).read_bytes() == (folder / "job" / filename).read_bytes(), filename
        identity_lines = re.findall(r"^(?:#{1,6}[ \t]+)?(?:姓名|准考证号|座位号|考场号|班级|学校)[ \t]*$", markdown, re.M)
        leaked = [] if args.move_to_body else identity_lines
        # 隐藏正文不能靠删除结果实现：逐一核对旁注的原始输出、状态和源类别。
        def aside_blocks(p):
            layouts = {l['id']: l for l in p['layout_blocks']}
            regions = {r['id']: r for r in p['regions']}
            result = {}
            for b in p['blocks']:
                layout = layouts[regions[b['source_region_ids'][0]]['source_layout_block_ids'][0]]
                if layout.get('original_class_id') == 2:
                    result[layout['candidate_id']] = (b, layout)
            return result
        actual_asides = aside_blocks(actual['pages'][0])
        expected_asides = aside_blocks(page) if args.mode == 'original' else {}
        if args.mode == 'minimal' and args.class_id in (None, 2):
            expected = copy.deepcopy(block)
            if args.plain_name:
                expected['content']['text'] = expected['provenance']['raw_output'] = '姓名\n'
            expected_asides = {0: (expected, layout)}
        retained_metadata = set(actual_asides) == set(expected_asides)
        for cid, (expected, source_layout) in expected_asides.items():
            if cid not in actual_asides:
                retained_metadata = False
                continue
            b, layout = actual_asides[cid]
            retained_metadata &= (b['provenance']['raw_output'] == expected['provenance']['raw_output'] and
                                  b['content']['text'] == expected['content']['text'] and
                                  b['status'] == expected['status'] and layout['model_label'] == source_layout['model_label'] and
                                  b.get('block_order') is None and (folder / 'job' / b['content']['resource']).is_file())
            if args.mode == 'original':
                retained_metadata &= (b['bbox'] == expected['bbox'] and layout['original_bbox'] == source_layout['original_bbox'] and
                                      sha(folder / 'job' / b['content']['resource']) == sha(source / expected['content']['resource']))
        instruction = "考生务必将自己的姓名、准考证号填写在答题卡上"
        retained_instruction = instruction in markdown if args.mode == "original" else None
        observation = [dict(type=b["type"], bbox=b["bbox"],
                            in_reading_order=b["id"] in actual["pages"][0]["reading_order"],
                            content_role=b.get("content_role"), text=b["content"]["text"])
                       for b in actual["pages"][0]["blocks"]] if args.mode != "original" else []
        result = dict(case=name, mode=args.mode, verdict="FAIL" if leaked or retained_instruction is False or not retained_metadata else "PASS",
                      leaked_identity_lines=leaked, instruction_retained=retained_instruction,
                      aside_block_count=len(actual_asides), aside_metadata_retained=bool(retained_metadata),
                      probes=dict(class_id=args.class_id, move_to_body=args.move_to_body, plain_name=args.plain_name),
                      observations=observation,
                      block_count=len(actual["pages"][0]["blocks"]), elapsed_seconds=round(time.monotonic()-start, 3),
                      command=command, source_document_sha256=sha(source / "document.json"),
                      cli_sha256=sha(args.cli), library_sha256=sha(args.cli.parent / "libdococr_c_test.so"),
                      production_cli_sha256=sha(args.production_cli), reexport_consistent=True,
                      execution="captured_outputs_replay_not_fresh_model_inference")
        results.append(result)
        print(json.dumps({k: result[k] for k in ["case", "mode", "verdict", "leaked_identity_lines",
                                                "instruction_retained", "elapsed_seconds", "probes", "observations"]}, ensure_ascii=False))
    write(output / "report.json", results)
    print("诊断产物：" + str(output))
    return int(any(r["verdict"] == "FAIL" for r in results))


if __name__ == "__main__":
    sys.exit(main())
