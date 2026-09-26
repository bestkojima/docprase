#!/usr/bin/env python3
"""按同图哈希对照 MNN、原框架与数据集标注，保留逐字差异。"""
import argparse
import difflib
import hashlib
import json
from pathlib import Path


NAMES = ("chinese_text", "printed_formula", "complete_table")


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mnn-suite", type=Path, required=True)
    parser.add_argument("--original-text", type=Path, required=True)
    parser.add_argument("--original-formula", type=Path, required=True)
    parser.add_argument("--original-table", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    mnn_manifest = json.loads((args.mnn_suite / "artifact-manifest.json").read_text())
    original = dict(zip(NAMES, (args.original_text, args.original_formula, args.original_table)))
    comparison = {"kind": "same_image_same_prompt_original_framework_vs_MNN",
                  "samples": {}, "all_original_runs_normal": True,
                  "prompt_sha256": hashlib.sha256(mnn_manifest["prompt"].encode()).hexdigest(),
                  "max_tokens": mnn_manifest["max_tokens"]}
    for name in NAMES:
        mnn_report = json.loads((args.mnn_suite / name / "report.json").read_text())
        ref_report = json.loads((original[name] / "report.json").read_text())
        if mnn_report["image_sha256"] != ref_report["image_sha256"]:
            raise ValueError("图像哈希不相同: " + name)
        if ref_report["prompt"] != mnn_manifest["prompt"] or ref_report["max_new_tokens"] != mnn_manifest["max_tokens"]:
            raise ValueError("提示词或 token 上限不相同: " + name)
        mnn_raw = (args.mnn_suite / name / "raw.txt").read_text()
        original_raw = (original[name] / "raw.txt").read_text()
        if sha256(args.mnn_suite / name / "raw.txt") != mnn_report["raw_sha256"]:
            raise ValueError("MNN 原始输出哈希不符: " + name)
        if sha256(original[name] / "raw.txt") != ref_report["raw_sha256"]:
            raise ValueError("原框架输出哈希不符: " + name)
        diff = "".join(difflib.unified_diff([line + "\n" for line in original_raw.splitlines()],
                                            [line + "\n" for line in mnn_raw.splitlines()],
                                            fromfile="original_framework", tofile="mnn"))
        (args.out / (name + ".original-vs-mnn.diff")).write_text(diff)
        normal = ref_report["stop_reason"] == "normal"
        comparison["all_original_runs_normal"] &= normal
        comparison["samples"][name] = {
            "image_sha256": mnn_report["image_sha256"],
            "original_stop_reason": ref_report["stop_reason"],
            "mnn_stop_reason": mnn_report["stop_reason"],
            "original_generated_tokens": ref_report["generated_tokens"],
            "mnn_generated_tokens": mnn_report["runtime"]["tokens"],
            "original_raw_sha256": ref_report["raw_sha256"],
            "mnn_raw_sha256": mnn_report["raw_sha256"],
            "byte_exact": original_raw == mnn_raw,
            "equal_ignoring_terminal_newline": original_raw.rstrip("\n") == mnn_raw.rstrip("\n"),
            "diff": name + ".original-vs-mnn.diff",
        }
    (args.out / "report.json").write_text(json.dumps(comparison, ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    main()
