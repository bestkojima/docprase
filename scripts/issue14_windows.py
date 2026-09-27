"""Issue #14: Windows 模型获取和公共 CLI/C ABI 实测证据。"""

from __future__ import annotations

import argparse
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
MODEL_HASHES = {
    "doclayout/PP-DocLayoutV3.mnn": "5f1a43441d70f6843012b47eb294bed7edd3d0ef2344f0074700a38cb2e29c67",
    "ovis/config.json": "b81ac7008ba5f894301b7b9265bba882889df52c6e25c86390514c1bd4afe0c4",
    "ovis/llm_config.json": "bb0d93883767c2c47de7f6965494c689c9890fffb79e3ca5e00e6f1fa5fd9770",
    "ovis/llm.mnn": "1da84439dec62e4f966833c54438859e1bdae8a07b03a55c643aabbeb48bc880",
    "ovis/llm.mnn.weight": "f09832b6ee9d63167ef456e83dea7f5df28e3983953e28b0a1e63e598cb45d32",
    "ovis/visual.mnn": "d85dbe1c24890bdd514cf35f23d2c0dfdb3d207489f50c05705782759881daa5",
    "ovis/visual.mnn.weight": "2a1b5138bdc1f6369df64f2b21b379b11f47ec51a46bb74a5013e4b2b9c4aa6d",
    "ovis/tokenizer.mtok": "1a0c1ee1d04a63791ea87be31bb8c1f6346ecdd9c3b55ccdbb894cea32fbedfa",
    "ovis/export_args.json": "953745b0456e0e4e3d2d4a0b31dd5a2d4dce632230a0b1d7c0ccfeead2de9334",
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def model_manifest(model_root: Path) -> dict[str, str]:
    result = {}
    for name, expected in MODEL_HASHES.items():
        actual = sha256(model_root / name)
        assert actual == expected, f"模型哈希错误：{name}: {actual}"
        result[name] = actual
    return result


def download(model_root: Path) -> None:
    from modelscope import snapshot_download

    model_root.mkdir(parents=True, exist_ok=True)
    snapshot_download("dr3334/PP-DocLayoutV3-mnn",
                      revision="c67c1a858d5f6c855172d4cfdf931798dafa2edd",
                      local_dir=str(model_root / "doclayout"))
    snapshot_download("dr3334/ovrics-ocrv2_mnn",
                      revision="20f12e49d846941e67829a7a7c3645693e485942",
                      local_dir=str(model_root / "ovis"))
    print(json.dumps(model_manifest(model_root), indent=2))


class View(c.Structure):
    _fields_ = [("data", c.c_char_p), ("size", c.c_size_t)]


class Bytes(c.Structure):
    _fields_ = [("data", c.POINTER(c.c_uint8)), ("size", c.c_size_t),
                ("allocation_id", c.c_uint64)]


class Result(c.Structure):
    _fields_ = [("struct_size", c.c_uint32), ("json", Bytes), ("markdown", Bytes)]


class OldInput(c.Structure):
    _fields_ = [("struct_size", c.c_uint32), ("data", c.POINTER(c.c_uint8)),
                ("size", c.c_size_t), ("format", c.c_uint32),
                ("width", c.c_uint32), ("height", c.c_uint32),
                ("row_stride", c.c_size_t)]


def check_abi(dll: Path, config: bytes) -> dict:
    dll_directory = os.add_dll_directory(str(dll.parent))
    lib = c.CDLL(str(dll))
    lib.dococr_abi_version.restype = c.c_uint32
    lib.dococr_create.argtypes = [View, c.POINTER(c.c_uint64)]
    lib.dococr_last_error.argtypes = [c.POINTER(Bytes)]
    lib.dococr_execution_plan.argtypes = [c.c_uint64, c.POINTER(Bytes)]
    lib.dococr_job_create.argtypes = [c.c_uint64, c.POINTER(c.c_uint64)]
    lib.dococr_job_run.argtypes = [c.c_uint64, c.POINTER(OldInput)]
    lib.dococr_job_result.argtypes = [c.c_uint64, c.POINTER(Result)]
    lib.dococr_job_destroy.argtypes = [c.c_uint64]
    lib.dococr_destroy.argtypes = [c.c_uint64]
    lib.dococr_bytes_free.argtypes = [c.POINTER(Bytes)]
    assert lib.dococr_abi_version() == 1
    assert lib.dococr_job_destroy(987654321) == 2

    invalid = b"mnn:pp-doclayout-v3+ovisocr2"
    handle = c.c_uint64()
    assert lib.dococr_create(View(invalid, len(invalid)), c.byref(handle)) == 9
    error = Bytes()
    assert lib.dococr_last_error(c.byref(error)) == 0
    message = json.loads(c.string_at(error.data, error.size).decode("utf-8"))
    assert message["code"] == "configuration_required"
    stale = Bytes(error.data, error.size, error.allocation_id)
    assert lib.dococr_bytes_free(c.byref(error)) == 0
    assert lib.dococr_bytes_free(c.byref(stale)) == 1

    assert lib.dococr_create(View(config, len(config)), c.byref(handle)) == 0
    plan = Bytes()
    assert lib.dococr_execution_plan(handle, c.byref(plan)) == 0
    effective = json.loads(c.string_at(plan.data, plan.size).decode("utf-8"))
    assert lib.dococr_bytes_free(c.byref(plan)) == 0
    job = c.c_uint64()
    assert lib.dococr_job_create(handle, c.byref(job)) == 0
    old_result = Result(0)
    assert lib.dococr_job_result(job, c.byref(old_result)) == 1
    old_input = OldInput(c.sizeof(OldInput), None, 0, 1, 0, 0, 0)
    old_input_status = lib.dococr_job_run(job, c.byref(old_input))
    assert old_input_status == 5, f"旧版输入结构应进入解码并返回输入错误：{old_input_status}"
    assert lib.dococr_job_destroy(job) == 0
    assert lib.dococr_destroy(handle) == 0
    assert lib.dococr_destroy(handle) == 2
    dll_directory.close()
    return {"abi_version": 1, "error": message, "stale_free_status": 1,
            "invalid_handle_status": 2, "old_struct_status": 1,
            "old_input_struct_size": c.sizeof(OldInput),
            "old_input_run_status": old_input_status,
            "paired_job_and_engine_destroy": True,
            "effective_plan": effective}


def verify(cli: Path, dll: Path, model_root: Path, output: Path) -> None:
    assert sys.platform == "win32", "必须在真实 Windows 环境运行"
    output.mkdir(parents=True, exist_ok=True)
    hashes = model_manifest(model_root)
    source = ROOT / "tests/fixtures/ovis/source_page.jpg"
    assert sha256(source) == "c8cf71eb2f717727dc2d8a3ae5da1e388f6be7bb1e2c4addbde5d40dafb270f6"
    chinese_source = output / "教材样例.jpg"
    shutil.copyfile(source, chinese_source)
    job = output / "中文解析结果"
    command = [str(cli), "--config", "configs/printed-page.example.json",
               "--input", str(chinese_source), "--out", str(job)]
    run = subprocess.run(command, cwd=ROOT, capture_output=True, timeout=1800)
    (output / "cli.stdout.log").write_bytes(run.stdout)
    (output / "cli.stderr.log").write_bytes(run.stderr)
    assert run.returncode == 0, f"CLI 退出码 {run.returncode}：{run.stderr.decode('utf-8', 'replace')}"
    document_bytes = (job / "document.json").read_bytes()
    markdown_bytes = (job / "document.md").read_bytes()
    document = json.loads(document_bytes.decode("utf-8", "strict"))
    markdown = markdown_bytes.decode("utf-8", "strict")
    manifest = json.loads((job / "run-manifest.json").read_text(encoding="utf-8"))
    plan = json.loads((job / "execution-plan.json").read_text(encoding="utf-8"))
    import jsonschema
    schemas = {"1.0": "issue-4", "1.1": "issue-8", "1.2": "issue-9",
               "1.3": "issue-10", "1.4": "issue-11"}
    version = document["schema_version"]
    schema = json.loads((ROOT / f"docs/{schemas[version]}/document-ir-{version}.schema.json")
                        .read_text(encoding="utf-8"))
    jsonschema.validate(document, schema)
    blocks = document["pages"][0]["blocks"]
    assert blocks and manifest["actual_device"] == "cpu"
    assert manifest["backend_id"] == "mnn:pp-doclayout-v3+ovisocr2"
    assert len(manifest["regions"]) == len(blocks)
    assert any(block["type"] == "text" and block["content"].get("text") for block in blocks)
    resources = document["resources"]
    assert resources
    resource_paths = {item["path"] for item in resources}
    for item in resources:
        relative = Path(item["path"])
        assert not relative.is_absolute() and ".." not in relative.parts
        assert (job / relative).read_bytes().startswith(b"\x89PNG\r\n\x1a\n")
    markdown_paths = set(re.findall(r"assets/[A-Za-z0-9_./-]+\.png", markdown))
    assert markdown_paths and markdown_paths <= resource_paths
    reference = (ROOT / "tests/fixtures/ovis/chinese_text.reference.txt").read_text(encoding="utf-8").strip()
    reference_matches = [block["id"] for block in blocks if
                         block["type"] == "text" and
                         block["content"].get("text", "").strip() == reference]
    abi = check_abi(dll, (ROOT / "configs/printed-page.example.json").read_bytes())
    (output / "abi-result.json").write_text(json.dumps(abi, ensure_ascii=False, indent=2), encoding="utf-8")
    summary = {
        "platform": platform.platform(), "python": sys.version,
        "command": command, "cli_exit_code": run.returncode,
        "input_name": chinese_source.name, "output_name": job.name,
        "source_sha256": sha256(chinese_source), "model_sha256": hashes,
        "document_sha256": hashlib.sha256(document_bytes).hexdigest(),
        "markdown_sha256": hashlib.sha256(markdown_bytes).hexdigest(),
        "status": document["status"], "block_count": len(blocks),
        "reference_matches": reference_matches,
        "raw_outputs": {block["id"]: block.get("provenance", {}).get("raw_output")
                        for block in blocks if block.get("provenance", {}).get("raw_output")},
        "resource_count": len(resources), "config_hash": manifest["config_hash"],
        "runtime_configuration": manifest.get("runtime_configuration"),
        "effective_parameters": manifest.get("effective_parameters"),
        "processing": manifest.get("processing"),
        "effective_plan_sha256": hashlib.sha256(json.dumps(plan, sort_keys=True).encode()).hexdigest(),
        "abi": {key: value for key, value in abi.items() if key != "effective_plan"},
    }
    (output / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="action", required=True)
    get = sub.add_parser("download")
    get.add_argument("--models", type=Path, default=ROOT / "models")
    check = sub.add_parser("verify")
    check.add_argument("--cli", type=Path, required=True)
    check.add_argument("--dll", type=Path, required=True)
    check.add_argument("--models", type=Path, default=ROOT / "models")
    check.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if args.action == "download":
        download(args.models)
    else:
        verify(args.cli.resolve(), args.dll.resolve(), args.models, args.out)
