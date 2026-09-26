"""公共 CLI 的配置、计划与运行清单验收；固定后端仅提供编排证据。"""
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
import copy
from cli_integration import png_2x2


FLOW = [
    {"id": "load", "processor": "page_loader", "depends_on": []},
    {"id": "layout", "skill": "layout.detect", "depends_on": ["load"]},
    {"id": "recognize", "skill": "ocr.transcribe", "depends_on": ["layout"]},
    {"id": "assemble", "processor": "document_assembler", "depends_on": ["recognize"]},
    {"id": "export", "processor": "document_exporter", "depends_on": ["assemble"]},
]


def config(root):
    artifact = root / "contract.bin"
    artifact.write_bytes(b"fixture contract artifact\n")
    model = {"contract_status": "verified_fixture", "root": str(root), "artifacts": [
        {"path": artifact.name, "sha256": hashlib.sha256(artifact.read_bytes()).hexdigest()}]}
    return {"schema_version": "1.0", "mode": "development", "backend": "fixture:normalized",
            "models": {"layout": model, "recognition": model},
            "skills": {"layout.detect": "layout", "ocr.transcribe": "recognition"},
            "flow": FLOW, "processing": [
                {"id": "decode", "owner": "adapter", "enabled": True},
                {"id": "crop", "owner": "adapter", "enabled": True},
                {"id": "normalize", "owner": "adapter", "enabled": True},
                {"id": "session_reset", "owner": "runtime", "enabled": True},
                {"id": "rgb_identity", "owner": "none", "enabled": True},
                {"id": "deskew", "owner": "none", "enabled": False}],
            "platform": {"device": "cpu", "threads": 1},
            "execution": {"max_page_pixels": 16000000, "max_output_bytes": 1048576,
                          "max_new_tokens": 4096}}


def invoke(binary, cfg, source, root):
    path = root / "config.json"
    path.write_text(" \n" + json.dumps(cfg), encoding="utf-8")
    return subprocess.run([binary, "--config", str(path), "--input", str(source),
                           "--out", str(root / "out")], capture_output=True, text=True)


def main():
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        source = root / "page.png"
        source.write_bytes(png_2x2())
        good = config(root)
        result = invoke(binary, good, source, root)
        assert result.returncode == 0, result.stderr
        plan = json.loads((root / "out/execution-plan.json").read_text())
        manifest = json.loads((root / "out/run-manifest.json").read_text())
        trace = {step["id"]: step for step in manifest["processing"]}
        assert plan["config_hash"] == manifest["config_hash"]
        canonical = json.dumps(good, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
        assert plan["config_hash"] == hashlib.sha256(canonical.encode()).hexdigest()
        assert [step["output_kind"] for step in plan["resolved_flow"]] == [
            "page", "layout", "fragments", "document", "output"]
        assert manifest["actual_backend"] == "test_fixture"
        assert manifest["actual_device"] == "cpu"
        assert trace["decode"]["status"] == "executed"
        assert trace["normalize"]["status"] == "executed"
        assert trace["session_reset"]["status"] == "delegated_runtime"
        assert trace["rgb_identity"]["status"] == "identity_validated"
        assert trace["deskew"]["status"] == "skipped_disabled"
        assert manifest["metrics"]["peak_memory_bytes"]["status"] == "unavailable"
        bad = json.loads(json.dumps(good))
        bad["mystery"] = 1
        result = invoke(binary, bad, source, root)
        assert result.returncode == 3 and "unknown_field" in result.stderr
        bad = json.loads(json.dumps(good))
        bad["execution"]["max_page_pixels"] = 1
        result = invoke(binary, bad, source, root)
        assert result.returncode == 5 and "budget_exceeded" in result.stderr
        cases = []
        bad = copy.deepcopy(good); bad["processing"][0]["mystery"] = 1
        cases.append((bad, "unknown_field"))
        bad = copy.deepcopy(good); bad["flow"][0]["processor"] = "imaginary"
        cases.append((bad, "unknown_processor"))
        bad = copy.deepcopy(good); del bad["skills"]["ocr.transcribe"]
        cases.append((bad, "missing_field"))
        bad = copy.deepcopy(good); bad["flow"][0]["depends_on"] = ["export"]
        cases.append((bad, "cycle_or_missing_dependency"))
        bad = copy.deepcopy(good); bad["flow"][2]["depends_on"] = ["load"]
        cases.append((bad, "type_disconnected"))
        bad = copy.deepcopy(good); bad["processing"][0]["enabled"] = False
        cases.append((bad, "required_processing_disabled"))
        bad = copy.deepcopy(good); bad["processing"].append(copy.deepcopy(bad["processing"][2]))
        cases.append((bad, "duplicate_processing"))
        bad = copy.deepcopy(good); bad["processing"][2]["owner"] = "runtime"
        cases.append((bad, "processing_owner_mismatch"))
        bad = copy.deepcopy(good); bad["models"]["layout"]["artifacts"][0]["path"] = "missing.bin"
        cases.append((bad, "artifact_missing"))
        bad = copy.deepcopy(good); bad["models"]["layout"]["artifacts"][0]["sha256"] = "0" * 64
        cases.append((bad, "artifact_hash_mismatch"))
        bad = copy.deepcopy(good); bad["mode"] = "production"
        cases.append((bad, "contract_unverified"))
        bad = copy.deepcopy(good); bad["platform"]["threads"] = 4294967297
        cases.append((bad, "unsupported_parameter"))
        for bad, code in cases:
            result = invoke(binary, bad, source, root)
            assert result.returncode == 3 and code in result.stderr, (code, result.stderr)

        for backend, owner, marker, status in [
            ("fixture:runtime", "runtime", "运行时归一化", "delegated_runtime"),
            ("fixture:graph", "graph", "图内归一化", "provided_by_graph"),
        ]:
            cfg = copy.deepcopy(good)
            cfg["backend"] = backend
            cfg["processing"][2]["owner"] = owner
            result = invoke(binary, cfg, source, root)
            assert result.returncode == 0, result.stderr
            assert marker in (root / "out/document.md").read_text()
            manifest = json.loads((root / "out/run-manifest.json").read_text())
            assert {step["id"]: step for step in manifest["processing"]}["normalize"]["status"] == status
            cfg["execution"]["max_new_tokens"] = 1
            result = invoke(binary, cfg, source, root)
            assert result.returncode == 0, result.stderr
            assert json.loads((root / "out/document.json").read_text())["status"] == "partial"

        international = root / "中文😀"
        international.mkdir()
        named = international / "契约😀.bin"
        named.write_bytes(b"fixture contract artifact\n")
        cfg = copy.deepcopy(good)
        for model in cfg["models"].values():
            model["root"] = str(international)
            model["artifacts"][0]["path"] = named.name
        result = invoke(binary, cfg, source, root)
        assert result.returncode == 0, result.stderr

        cfg = copy.deepcopy(good)
        cfg["execution"]["max_output_bytes"] = 1
        result = invoke(binary, cfg, source, root)
        assert result.returncode == 5 and "budget_exceeded" in result.stderr
        cfg = copy.deepcopy(good)
        cfg["execution"]["max_new_tokens"] = 1
        result = invoke(binary, cfg, source, root)
        assert result.returncode == 0, result.stderr
        assert json.loads((root / "out/document.json").read_text())["status"] == "partial"
        for backend, expected_reset, expected_crop in [
            ("fixture:reset_failure", "failed", "executed"),
            ("fixture:blank", "identity_validated", "identity_validated"),
        ]:
            cfg = copy.deepcopy(good)
            cfg["backend"] = backend
            result = invoke(binary, cfg, source, root)
            assert result.returncode == 0, result.stderr
            trace = {step["id"]: step for step in
                     json.loads((root / "out/run-manifest.json").read_text())["processing"]}
            assert trace["session_reset"]["status"] == expected_reset
            assert trace["crop"]["status"] == expected_crop


if __name__ == "__main__":
    main()
