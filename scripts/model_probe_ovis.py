#!/usr/bin/env python3
"""独立 Ovis 区域探测入口；每次请求用新进程和新模型实例。"""
import argparse
import difflib
import hashlib
from html.parser import HTMLParser
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MODEL = ROOT / "models/ovis"
DEFAULT_MNN = ROOT.parent / "MNN"
FIXTURES = ROOT / "tests/fixtures/ovis"
PROMPT = (FIXTURES / "prompt.txt").read_text().rstrip("\n")
MODEL_FILES = ("config.json", "llm_config.json", "llm.mnn", "llm.mnn.weight",
               "visual.mnn", "visual.mnn.weight", "tokenizer.mtok", "export_args.json")


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")


def build_runner(mnn):
    source = ROOT / "scripts/model_probe_ovis.cpp"
    binary = ROOT / "output/model-probe-ovis/model_probe_ovis"
    binary.parent.mkdir(parents=True, exist_ok=True)
    # Small diagnostic binary: rebuild so a changed MNN root/library cannot reuse a stale probe.
    subprocess.run([
        "g++", "-std=c++17", "-O2", "-I", str(mnn / "transformers/llm/engine/include"),
        "-I", str(mnn / "include"), str(source), "-L", str(mnn / "build"),
        "-Wl,-rpath," + str(mnn / "build"), "-lllm", "-lMNN", "-pthread", "-ldl",
        "-o", str(binary),
    ], check=True)
    return binary


def effective_config(model, out):
    config = json.loads((model / "config.json").read_text())
    config.update({"base_dir": str(model.resolve()) + "/", "sampler_type": "greedy",
                   "reuse_kv": False, "prompt_cache": False, "use_mmap": False,
                   "kvcache_mmap": False, "async": False})
    path = out / "effective-config.json"
    write_json(path, config)
    return path


def stop_reason(status, exit_code, timed_out):
    if timed_out:
        return "timeout"
    if exit_code != 0:
        return "error"
    return {"NORMAL_FINISHED": "normal", "MAX_TOKENS_FINISHED": "token_limit",
            "TIMEOUT": "timeout", "USER_CANCEL": "cancelled",
            "INTERNAL_ERROR": "error"}.get(status, "error")


def process_group_has_live_members(group_id):
    """Linux 上忽略等待 init 回收的 zombie，检测仍会运行的推理成员。"""
    for stat in Path("/proc").glob("[0-9]*/stat"):
        try:
            fields = stat.read_text().rsplit(") ", 1)[1].split()
            if int(fields[2]) == group_id and fields[0] != "Z":
                return True
        except (FileNotFoundError, IndexError, ValueError, PermissionError):
            continue
    return False


class TableShape(HTMLParser):
    def __init__(self):
        super().__init__()
        self.rows = []
        self.in_row = False

    def handle_starttag(self, tag, attrs):
        if tag == "tr":
            self.rows.append(0)
            self.in_row = True
        elif tag in ("td", "th") and self.in_row:
            self.rows[-1] += 1

    def handle_endtag(self, tag):
        if tag == "tr":
            self.in_row = False


def reference_format_check(kind, raw, expected):
    if kind == "printed_formula":
        text = raw.strip()
        latex_delimited = ((text.startswith("$$") and text.endswith("$$") and len(text) > 4)
                           or (text.startswith("$") and text.endswith("$") and len(text) > 2
                               and text.count("$") == 2))
        return {"valid": latex_delimited,
                "requirement": "LaTeX 公式标记"}
    if kind == "complete_table":
        actual = TableShape()
        target = TableShape()
        actual.feed(raw)
        target.feed(expected)
        valid = ("<table" in raw.lower() and "</table>" in raw.lower()
                 and raw.lower().count("<tr") == raw.lower().count("</tr>")
                 and raw.lower().count("<td") == raw.lower().count("</td>")
                 and actual.rows == target.rows and bool(actual.rows))
        return {"valid": valid, "requirement": "完整 HTML 表及标注对应的行列数",
                "actual_cells_per_row": actual.rows, "reference_cells_per_row": target.rows}
    return {"valid": bool(raw.strip()), "requirement": "非空正文"}


def run_one(args, runner, config, image, out, reference=None):
    out.mkdir(parents=True, exist_ok=True)
    raw = out / "raw.txt"
    command = [str(runner), str(config), str(image.resolve()), PROMPT,
               str(args.max_tokens), str(raw)]
    started = time.monotonic()
    process = subprocess.Popen(command, cwd=out, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, start_new_session=True)
    timed_out = False
    group_terminated = False
    try:
        stdout, stderr = process.communicate(timeout=args.timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            stdout, stderr = process.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            stdout, stderr = process.communicate()
        group_terminated = not process_group_has_live_members(process.pid)
    (out / "stdout.log").write_bytes(stdout)
    (out / "stderr.log").write_bytes(stderr)
    marker = next((line.removeprefix("OVIS_PROBE ") for line in stdout.decode("utf-8", "replace").splitlines()
                   if line.startswith("OVIS_PROBE ")), None)
    try:
        runtime = json.loads(marker) if marker else {}
    except json.JSONDecodeError:
        runtime = {}
    reason = stop_reason(runtime.get("status"), process.returncode, timed_out)
    vision = runtime.get("vision_us", 0) > 0 and runtime.get("pixels_mp", 0) > 0
    if not timed_out and not vision:
        reason = "error"
    content = raw.read_text(errors="replace") if raw.exists() else ""
    completeness = "complete" if reason == "normal" and vision and content.strip() else (
        "invalid" if reason == "normal" else "incomplete")
    report = {"image": str(image.resolve()), "image_sha256": digest(image),
              "command": command, "exit_code": process.returncode,
              "elapsed_seconds": round(time.monotonic() - started, 3),
              "stop_reason": reason, "completeness": completeness,
              "runtime": runtime, "visual_processing_confirmed": vision,
              "process_group_terminated": group_terminated if timed_out else None,
              "raw_sha256": digest(raw) if raw.exists() else None,
              "raw_bytes": raw.stat().st_size if raw.exists() else 0}
    if reference:
        expected = reference.read_text()
        report["reference"] = {"path": str(reference.resolve()), "sha256": digest(reference),
                               "exact_match": content == expected}
        diff = "".join(difflib.unified_diff(expected.splitlines(keepends=True),
                                            content.splitlines(keepends=True),
                                            fromfile="reference", tofile="mnn_raw"))
        (out / "reference.diff").write_text(diff)
    write_json(out / "report.json", report)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--image", type=Path)
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--suite", action="store_true")
    parser.add_argument("--model-dir", type=Path, default=DEFAULT_MODEL)
    parser.add_argument("--mnn-root", type=Path, default=DEFAULT_MNN)
    parser.add_argument("--runner", type=Path)
    parser.add_argument("--max-tokens", type=int, default=1024)
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    if args.max_tokens < 1 or args.timeout < 1 or args.suite == bool(args.image):
        parser.error("指定 --suite 或 --image 其一，并使用正数限额")
    runner = args.runner.resolve() if args.runner else build_runner(args.mnn_root.resolve())
    model = args.model_dir.resolve()
    config = effective_config(model, args.out)
    manifest = {name: digest(model / name) for name in MODEL_FILES}
    write_json(args.out / "artifact-manifest.json", {
        "model_dir": str(model), "files_sha256": manifest,
        "effective_config_sha256": digest(config), "runner_sha256": digest(runner),
        "mnn_git_commit": subprocess.run(["git", "rev-parse", "HEAD"], cwd=args.mnn_root,
                                          capture_output=True, text=True).stdout.strip(),
        "prompt": PROMPT, "max_tokens": args.max_tokens,
        "timeout_seconds": args.timeout, "runner_override": bool(args.runner),
        "session_strategy": "new_process_and_model_instance_per_region",
    })
    if not args.suite:
        result = run_one(args, runner, config, args.image, args.out, args.reference)
        return 0 if result["stop_reason"] not in ("error", "timeout") else 1
    samples = json.loads((FIXTURES / "manifest.json").read_text())["samples"]
    results = {}
    for sample in samples:
        name = sample["name"]
        image = FIXTURES / sample["image"]
        if digest(image) != sample["image_sha256"]:
            raise ValueError("样本哈希不匹配: " + name)
        results[name] = run_one(args, runner, config, image, args.out / name,
                                FIXTURES / sample["reference"])
        raw = (args.out / name / "raw.txt").read_text(errors="replace")
        expected = (FIXTURES / sample["reference"]).read_text()
        results[name]["format_check"] = reference_format_check(name, raw, expected)
        write_json(args.out / name / "report.json", results[name])
    # A→B uses the same process reconstruction policy as independent B.
    a = FIXTURES / samples[0]["image"]
    b = FIXTURES / samples[2]["image"]
    results["isolation_A"] = run_one(args, runner, config, a, args.out / "isolation_A")
    results["isolation_B"] = run_one(args, runner, config, b, args.out / "isolation_B")
    results["failure_bad_image"] = run_one(args, runner, config,
                                            FIXTURES / "invalid.png", args.out / "failure_bad_image")
    results["failure_then_B"] = run_one(args, runner, config, b, args.out / "failure_then_B")
    b_raw = (args.out / samples[2]["name"] / "raw.txt").read_bytes()
    summary = {"cases": {name: {key: value for key, value in result.items()
                                if key in ("stop_reason", "completeness", "raw_sha256", "exit_code",
                                           "visual_processing_confirmed", "format_check", "reference")}
                         for name, result in results.items()},
               "isolation_exact_match": b_raw == (args.out / "isolation_B/raw.txt").read_bytes(),
               "failure_recovery_exact_match": b_raw == (args.out / "failure_then_B/raw.txt").read_bytes(),
               "model_evidence": "real_mnn" if not args.runner else "synthetic_runner_not_model_evidence",
               "reference_model_output": "not_verified",
               "reference_note": "OmniDocBench 标注是人工/数据集真值，不是 OvisOCR2 原框架输出。"}
    sample_ok = all(results[s["name"]]["completeness"] == "complete"
                    and (s["name"] != "complete_table" or results[s["name"]]["format_check"]["valid"])
                    for s in samples)
    sequence_ok = all(results[name]["completeness"] == "complete"
                      and results[name]["visual_processing_confirmed"]
                      for name in ("isolation_A", "isolation_B", "failure_then_B"))
    failure_ok = (results["failure_bad_image"]["stop_reason"] == "error"
                  and not results["failure_bad_image"]["visual_processing_confirmed"])
    summary["checks"] = {"sample_generation_and_table_structure": sample_ok,
                         "sequence_completion_and_vision": sequence_ok,
                         "controlled_failure": failure_ok,
                         "isolation_exact_match": summary["isolation_exact_match"],
                         "failure_recovery_exact_match": summary["failure_recovery_exact_match"]}
    summary["quality_gaps"] = [s["name"] + ":format" for s in samples
                               if not results[s["name"]]["format_check"]["valid"]]
    summary["quality_gaps"] += [s["name"] + ":exact_reference" for s in samples
                                if not results[s["name"]]["reference"]["exact_match"]]
    summary["diagnostic_complete"] = all(summary["checks"].values())
    summary["overall"] = ("diagnostic_failed" if not summary["diagnostic_complete"] else
                          "diagnostic_complete_with_quality_gaps" if summary["quality_gaps"] else
                          "diagnostic_complete")
    write_json(args.out / "suite-report.json", summary)
    return 0 if summary["diagnostic_complete"] else 1


if __name__ == "__main__":
    sys.exit(main())
