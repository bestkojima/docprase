"""Issue #14：在 Linux 上验证生产 CLI、真实双模型和共享库 C ABI。"""

from __future__ import annotations

import argparse
import ctypes as c
import hashlib
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / "configs/printed-page.example.json"
SOURCE = ROOT / "tests/fixtures/ovis/source_page.jpg"
SOURCE_SHA = "c8cf71eb2f717727dc2d8a3ae5da1e388f6be7bb1e2c4addbde5d40dafb270f6"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def check_models() -> dict[str, str]:
    config = json.loads(CONFIG.read_text(encoding="utf-8"))
    hashes = {}
    for model in config["models"].values():
        for artifact in model["artifacts"]:
            path = ROOT / model["root"] / artifact["path"]
            actual = sha256(path)
            assert actual == artifact["sha256"], f"模型工件哈希错误：{path}"
            hashes[str(path.relative_to(ROOT))] = actual
    return hashes


class View(c.Structure):
    _fields_ = [("data", c.c_char_p), ("size", c.c_size_t)]


class Bytes(c.Structure):
    _fields_ = [("data", c.POINTER(c.c_uint8)), ("size", c.c_size_t),
                ("allocation_id", c.c_uint64)]


class Input(c.Structure):
    _fields_ = [("struct_size", c.c_uint32), ("data", c.POINTER(c.c_uint8)),
                ("size", c.c_size_t), ("format", c.c_uint32),
                ("width", c.c_uint32), ("height", c.c_uint32),
                ("row_stride", c.c_size_t), ("first_page", c.c_uint32),
                ("last_page", c.c_uint32), ("dpi", c.c_uint32),
                ("max_page_pixels", c.c_uint64), ("timeout_ms", c.c_uint32)]


class OldInput(c.Structure):
    _fields_ = Input._fields_[:7]


class Result(c.Structure):
    _fields_ = [("struct_size", c.c_uint32), ("json", Bytes), ("markdown", Bytes)]


def abi_verify(library: Path, output: Path, source: bytes, reference: str) -> dict:
    lib = c.CDLL(str(library))
    lib.dococr_abi_version.restype = c.c_uint32
    lib.dococr_create.argtypes = [View, c.POINTER(c.c_uint64)]
    lib.dococr_last_error.argtypes = [c.POINTER(Bytes)]
    lib.dococr_execution_plan.argtypes = [c.c_uint64, c.POINTER(Bytes)]
    lib.dococr_job_create.argtypes = [c.c_uint64, c.POINTER(c.c_uint64)]
    lib.dococr_job_run.argtypes = [c.c_uint64, c.c_void_p]
    lib.dococr_job_result.argtypes = [c.c_uint64, c.POINTER(Result)]
    lib.dococr_job_manifest.argtypes = [c.c_uint64, c.POINTER(Bytes)]
    lib.dococr_job_asset_count.argtypes = [c.c_uint64, c.POINTER(c.c_size_t)]
    lib.dococr_job_asset.argtypes = [c.c_uint64, c.c_size_t, c.POINTER(Bytes), c.POINTER(Bytes)]
    lib.dococr_job_destroy.argtypes = [c.c_uint64]
    lib.dococr_destroy.argtypes = [c.c_uint64]
    lib.dococr_bytes_free.argtypes = [c.POINTER(Bytes)]

    def read_and_free(value: Bytes) -> bytes:
        content = c.string_at(value.data, value.size)
        assert lib.dococr_bytes_free(c.byref(value)) == 0
        return content

    assert lib.dococr_abi_version() == 1
    assert lib.dococr_job_destroy(987654321) == 2
    invalid = b"mnn:pp-doclayout-v3+ovisocr2"
    engine = c.c_uint64()
    assert lib.dococr_create(View(invalid, len(invalid)), c.byref(engine)) == 9
    error_bytes = Bytes()
    assert lib.dococr_last_error(c.byref(error_bytes)) == 0
    stale = Bytes(error_bytes.data, error_bytes.size, error_bytes.allocation_id)
    error = json.loads(read_and_free(error_bytes).decode("utf-8", "strict"))
    assert error["code"] == "configuration_required"
    assert lib.dococr_bytes_free(c.byref(stale)) == 1

    config = CONFIG.read_bytes()
    assert lib.dococr_create(View(config, len(config)), c.byref(engine)) == 0
    plan_bytes = Bytes()
    assert lib.dococr_execution_plan(engine, c.byref(plan_bytes)) == 0
    plan = json.loads(read_and_free(plan_bytes).decode("utf-8", "strict"))
    job = c.c_uint64()
    assert lib.dococr_job_create(engine, c.byref(job)) == 0
    assert lib.dococr_destroy(engine) == 3
    too_small = Result(0)
    assert lib.dococr_job_result(job, c.byref(too_small)) == 1
    buffer = (c.c_uint8 * len(source)).from_buffer_copy(source)
    request = Input(c.sizeof(Input), buffer, len(source), 2, 0, 0, 0, 0, 0, 0, 0, 0)
    run_status = lib.dococr_job_run(job, c.byref(request))
    assert run_status == 0, f"生产 ABI 推理失败：{run_status}"
    result = Result(c.sizeof(Result))
    assert lib.dococr_job_result(job, c.byref(result)) == 0
    raw_json = read_and_free(result.json)
    raw_markdown = read_and_free(result.markdown)
    document = json.loads(raw_json.decode("utf-8", "strict"))
    raw_markdown.decode("utf-8", "strict")
    matching = [block["id"] for block in document["pages"][0]["blocks"] if
                block["type"] == "text" and block["content"].get("text", "").strip() == reference]
    assert matching
    manifest_bytes = Bytes()
    assert lib.dococr_job_manifest(job, c.byref(manifest_bytes)) == 0
    manifest = json.loads(read_and_free(manifest_bytes).decode("utf-8", "strict"))
    assert manifest["actual_device"] == "cpu"
    count = c.c_size_t()
    assert lib.dococr_job_asset_count(job, c.byref(count)) == 0
    assert count.value > 0
    name, data = Bytes(), Bytes()
    assert lib.dococr_job_asset(job, 0, c.byref(name), c.byref(data)) == 0
    first_name = read_and_free(name).decode("utf-8", "strict")
    first_asset = read_and_free(data)
    (output / "abi-document.json").write_bytes(raw_json)
    (output / "abi-document.md").write_bytes(raw_markdown)
    (output / "abi-manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    assert lib.dococr_job_destroy(job) == 0

    old_job = c.c_uint64()
    assert lib.dococr_job_create(engine, c.byref(old_job)) == 0
    old_input = OldInput(c.sizeof(OldInput), None, 0, 1, 0, 0, 0)
    old_status = lib.dococr_job_run(old_job, c.byref(old_input))
    assert old_status == 5, f"旧输入结构应进入解码并返回 INPUT_ERROR：{old_status}"
    assert lib.dococr_job_destroy(old_job) == 0
    assert lib.dococr_destroy(engine) == 0
    assert lib.dococr_destroy(engine) == 2
    return {"abi_version": 1, "invalid_handle_status": 2, "busy_destroy_status": 3,
            "old_result_struct_status": 1, "old_input_struct_size": c.sizeof(OldInput),
            "old_input_run_status": old_status, "stale_free_status": 1,
            "error": error, "job_run_status": run_status,
            "result_json_sha256": hashlib.sha256(raw_json).hexdigest(),
            "result_markdown_sha256": hashlib.sha256(raw_markdown).hexdigest(),
            "matched_reference_blocks": matching, "asset_count": count.value,
            "first_asset": first_name, "first_asset_sha256": hashlib.sha256(first_asset).hexdigest(),
            "effective_plan": plan, "runtime_configuration": manifest.get("runtime_configuration")}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--lib", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    assert sys.platform.startswith("linux"), "必须在实际 Linux 环境运行"
    cli, library, output = args.cli.resolve(), args.lib.resolve(), args.out.resolve()
    assert not output.exists(), "证据目录须为新目录，避免旧工件混入"
    output.mkdir(parents=True)
    hashes = check_models()
    assert sha256(SOURCE) == SOURCE_SHA
    chinese_source = output / "教材原图.jpg"
    shutil.copyfile(SOURCE, chinese_source)
    job = output / "中文教材页" / "job"
    command = [str(cli), "--config", "configs/printed-page.example.json",
               "--input", str(chinese_source), "--out", str(job)]
    run = subprocess.run(command, cwd=ROOT, capture_output=True, timeout=1800)
    (output / "cli.stdout.log").write_bytes(run.stdout)
    (output / "cli.stderr.log").write_bytes(run.stderr)
    assert run.returncode == 0, f"CLI 退出码 {run.returncode}: {run.stderr.decode('utf-8', 'replace')}"
    document_bytes = (job / "document.json").read_bytes()
    markdown_bytes = (job / "document.md").read_bytes()
    document = json.loads(document_bytes.decode("utf-8", "strict"))
    markdown = markdown_bytes.decode("utf-8", "strict")
    manifest = json.loads((job / "run-manifest.json").read_text(encoding="utf-8"))
    plan = json.loads((job / "execution-plan.json").read_text(encoding="utf-8"))
    import jsonschema
    version = document["schema_version"]
    schema_dir = {"1.0": "issue-4", "1.1": "issue-8", "1.2": "issue-9",
                  "1.3": "issue-10", "1.4": "issue-11"}[version]
    schema = json.loads((ROOT / f"docs/{schema_dir}/document-ir-{version}.schema.json")
                        .read_text(encoding="utf-8"))
    jsonschema.validate(document, schema)
    assert document["source"]["type"] == "image"
    assert manifest["actual_device"] == "cpu"
    assert manifest["backend_id"] == "mnn:pp-doclayout-v3+ovisocr2"
    blocks = document["pages"][0]["blocks"]
    reference = (ROOT / "tests/fixtures/ovis/chinese_text.reference.txt").read_text(encoding="utf-8").strip()
    matches = [block["id"] for block in blocks if block["type"] == "text" and
               block["content"].get("text", "").strip() == reference]
    assert matches, "固定教材正文未与参考文本匹配"
    resources = {asset["path"] for asset in document["resources"]}
    assert resources
    for relative in resources:
        path = Path(relative)
        assert not path.is_absolute() and ".." not in path.parts
        assert (job / path).is_file()
    markdown_refs = set(re.findall(r"assets/[A-Za-z0-9_./-]+\.png", markdown))
    assert markdown_refs and markdown_refs <= resources
    asset_files = sum(path.is_file() for path in (job / "assets").iterdir())
    assert asset_files >= len(resources)
    raw = next(block["provenance"]["raw_output"] for block in blocks if block["id"] == matches[0])
    (output / "raw-reference-block.txt").write_text(raw, encoding="utf-8")
    abi = abi_verify(library, output, chinese_source.read_bytes(), reference)
    assert abi["result_json_sha256"] == hashlib.sha256(document_bytes).hexdigest()
    assert abi["result_markdown_sha256"] == hashlib.sha256(markdown_bytes).hexdigest()
    (output / "abi-result.json").write_text(json.dumps(abi, ensure_ascii=False, indent=2), encoding="utf-8")
    summary = {"command": command, "cli_exit_code": run.returncode,
               "platform": platform.platform(), "source_sha256": SOURCE_SHA,
               "chinese_input": chinese_source.name, "chinese_output": job.parent.name,
               "document_status": document["status"], "block_count": len(blocks),
               "reference_matches": matches, "resources": len(resources),
               "asset_files": asset_files,
               "markdown_references": len(markdown_refs),
               "document_sha256": hashlib.sha256(document_bytes).hexdigest(),
               "markdown_sha256": hashlib.sha256(markdown_bytes).hexdigest(),
               "model_sha256": hashes, "config_hash": manifest["config_hash"],
               "runtime_configuration": manifest.get("runtime_configuration"),
               "effective_parameters": manifest.get("effective_parameters"),
               "processing": manifest.get("processing"),
               "abi_result": "abi-result.json"}
    (output / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
